// SPDX-License-Identifier: MIT
#include <cy/rendering/selection/outline_pass.h>

#include <cy/backends/rhi/validation.h>
#include <cy/rendering/graph/executor.h>

#include "selection_msl.h"
#include "selection_spirv.h"

#include <cstring>

namespace cy::rendering::selection {
namespace {

using rhi::Access;
using rhi::QueueKind;

/// selection_mask.slang's push word: the draw index below, the style slot above.
constexpr u32 kDrawIndexLimit = 1U << 24U;
constexpr u32 kSlotShift = 24U;
/// selection_outline.slang's set: mask, mask depth, scene depth, styles — the `ParameterBlock`'s
/// field order, which is also the argument-buffer id Metal assigns.
constexpr u32 kCompositeBindings = 4;
constexpr u64 kStyleBytes = static_cast<u64>(kMaxHighlightStyles) * sizeof(GpuOutlineStyle);
/// The readbacks, in `OutlineReadback`'s order.
constexpr u32 kReadbackMask = 0;
constexpr u32 kReadbackMaskDepth = 1;
constexpr u32 kReadbackSceneDepth = 2;
constexpr u32 kReadbackCount = 3;

template <typename T, usize N>
[[nodiscard]] Span<const u32> words(const T (&module)[N]) noexcept {
    return Span<const u32>(module, N);
}

template <usize N>
[[nodiscard]] Span<const u8> text(const char (&module)[N]) noexcept {
    return {reinterpret_cast<const u8*>(module), N - 1U};
}

struct StageSource {
    const char* name;
    rhi::ShaderStage stage;
    Span<const u32> spirv;
    Span<const u8> msl;
    const char* msl_entry;
};

[[nodiscard]] Expected<rhi::ShaderModuleHandle, Error> create_module(
    rhi::Device& device, const StageSource& source) noexcept {
    rhi::ShaderModuleBundle bundle;
    bundle.spirv = source.spirv;
    bundle.msl = source.msl;
    bundle.msl_entry_point = source.msl_entry;
    bundle.spirv_entry_point = "main";
    rhi::ValidationMessage message;
    Expected<rhi::ShaderModuleDescription, Error> module = rhi::select_shader_module(
        bundle, device.capabilities().native_shader_format(), source.name, source.stage, message);
    if (!module.has_value()) {
        return make_unexpected(module.error());
    }
    return device.create_shader_module(*module);
}

[[nodiscard]] rhi::DescriptorBinding fragment_binding(u32 binding,
                                                      rhi::DescriptorKind kind) noexcept {
    rhi::DescriptorBinding entry;
    entry.binding = binding;
    entry.kind = kind;
    entry.count = 1;
    entry.stages = rhi::ShaderStage::Fragment;
    return entry;
}

[[nodiscard]] rhi::DescriptorWrite sampled(u32 binding, rhi::TextureViewHandle view) noexcept {
    rhi::DescriptorWrite write;
    write.binding = binding;
    write.kind = rhi::DescriptorKind::SampledTexture;
    write.texture_view = view;
    write.use = rhi::ImageUse::SampledRead;
    return write;
}

[[nodiscard]] u64 image_bytes(const OutlinePassDescription& desc) noexcept {
    return static_cast<u64>(desc.width) * desc.height * 4U;
}

void set_full_viewport(rhi::CommandBuffer& commands, u32 width, u32 height) noexcept {
    commands.set_viewport(
        rhi::Viewport{0.0F, 0.0F, static_cast<f32>(width), static_cast<f32>(height), 0.0F, 1.0F});
    commands.set_scissor(rhi::Rect2D{0, 0, width, height});
}

void record_mask_fn(const PassContext& context, void* user) noexcept {
    static_cast<OutlinePass*>(user)->record_mask(context);
}

void record_composite_fn(const PassContext& context, void* user) noexcept {
    static_cast<OutlinePass*>(user)->record_composite(context);
}

void record_readback_fn(const PassContext& context, void* user) noexcept {
    static_cast<OutlinePass*>(user)->record_readback(context);
}

}  // namespace

OutlinePass::~OutlinePass() {
    destroy();
}

bool OutlinePass::supported(const rhi::Device& device) noexcept {
    const rhi::ShaderFormat format = device.capabilities().native_shader_format();
    return format == rhi::ShaderFormat::Spirv || format == rhi::ShaderFormat::Msl;
}

Status OutlinePass::create(rhi::Device& device, const pipeline::FramePipelines& pipelines,
                           const OutlinePassDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the selection outline pass is already created");
    }
    if (!supported(device)) {
        return fail(ErrorCode::Unsupported,
                    "selection outlines: this device takes no shader format this module ships "
                    "(SPIR-V or MSL)");
    }
    if (!pipelines.ready()) {
        return fail(ErrorCode::InvalidArgument,
                    "selection outlines: the frame's pipelines must be initialized first — the "
                    "mask draws through their sets 0 and 1");
    }
    if (desc.width == 0 || desc.height == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "OutlinePassDescription::width and ::height must be non-zero");
    }
    device_ = &device;
    desc_ = desc;
    depth_format_ = pipelines.setup().depth_format;
    // The composite lands on whatever the post chain ended in: the tonemapped output, or the
    // scene colour when the chain has no tone curve.
    output_format_ = pipelines.setup().tonemap ? pipelines.setup().output_format
                                               : pipelines.setup().color_format;
    Status made = create_mask_pipeline(pipelines);
    if (made) {
        made = create_composite_pipeline();
    }
    if (made) {
        made = create_buffers();
    }
    if (!made) {
        destroy();
    }
    return made;
}

Status OutlinePass::create_mask_pipeline(const pipeline::FramePipelines& pipelines) noexcept {
    const StageSource vertex{"cy selection mask vertex", rhi::ShaderStage::Vertex,
                             words(kOutlineMaskVertexSpirv), text(kOutlineMaskVertexMsl),
                             "cyOutlineMaskVertex"};
    const StageSource fragment{"cy selection mask fragment", rhi::ShaderStage::Fragment,
                               words(kOutlineMaskFragmentSpirv), text(kOutlineMaskFragmentMsl),
                               "cyOutlineMaskFragment"};
    Expected<rhi::ShaderModuleHandle, Error> module = create_module(*device_, vertex);
    if (!module.has_value()) {
        return make_unexpected(module.error());
    }
    mask_vertex_ = *module;
    module = create_module(*device_, fragment);
    if (!module.has_value()) {
        return make_unexpected(module.error());
    }
    mask_fragment_ = *module;

    // THE FRAME'S SETS 0 AND 1 AND THE FRAME'S PUSH RANGE, as the particle renderer takes them:
    // identical set layouts and an identical push range are what make this layout compatible with
    // the frame's for the sets it binds (Vulkan 14.2.2), and reusing the handles removes the
    // question of whether two descriptions agree.
    const rhi::DescriptorSetLayoutHandle sets[] = {pipelines.set_layout(pipeline::kGlobalSet),
                                                   pipelines.set_layout(pipeline::kViewSet)};
    const rhi::PushConstantRange range{rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
                                       sizeof(pipeline::DrawPush)};
    rhi::PipelineLayoutDescription layout;
    layout.name = "cy selection mask layout";
    layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(sets, 2);
    layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> made = device_->create_pipeline_layout(layout);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    mask_layout_ = *made;

    // The position stream alone: the mask needs where a surface is and nothing about it.
    const rhi::VertexBinding binding{pipeline::kPositionStream, pipeline::kPositionStreamStride,
                                     rhi::VertexInputRate::PerVertex};
    const rhi::VertexAttribute attribute{0, pipeline::kPositionStream, rhi::Format::Rgb32Sfloat, 0};
    rhi::ColorAttachmentState color;
    color.format = rhi::Format::R32Uint;
    rhi::GraphicsPipelineDescription description;
    description.name = "cy selection mask";
    description.layout = mask_layout_;
    description.vertex_shader = mask_vertex_;
    description.fragment_shader = mask_fragment_;
    description.vertex_bindings = Span<const rhi::VertexBinding>(&binding, 1);
    description.vertex_attributes = Span<const rhi::VertexAttribute>(&attribute, 1);
    description.color_attachments = Span<const rhi::ColorAttachmentState>(&color, 1);
    // The prepass's rasterisation, so a marked surface covers the pixels it covers there.
    description.rasterisation.cull_mode = rhi::CullMode::Back;
    description.rasterisation.front_face = rhi::FrontFace::CounterClockwise;
    description.depth_stencil.format = depth_format_;
    description.depth_stencil.depth_test_enable = true;
    description.depth_stencil.depth_write_enable = true;
    description.depth_stencil.depth_compare = rhi::CompareOp::GreaterOrEqual;
    Expected<rhi::GraphicsPipelineHandle, Error> created =
        device_->create_graphics_pipeline(description);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    mask_pipeline_ = *created;
    return ok();
}

Status OutlinePass::create_composite_pipeline() noexcept {
    const StageSource vertex{"cy selection outline vertex", rhi::ShaderStage::Vertex,
                             words(kOutlineVertexSpirv), text(kOutlineVertexMsl),
                             "cyOutlineVertex"};
    const StageSource fragment{"cy selection outline composite", rhi::ShaderStage::Fragment,
                               words(kOutlineCompositeSpirv), text(kOutlineCompositeMsl),
                               "cyOutlineComposite"};
    Expected<rhi::ShaderModuleHandle, Error> module = create_module(*device_, vertex);
    if (!module.has_value()) {
        return make_unexpected(module.error());
    }
    composite_vertex_ = *module;
    module = create_module(*device_, fragment);
    if (!module.has_value()) {
        return make_unexpected(module.error());
    }
    composite_fragment_ = *module;

    const rhi::DescriptorBinding bindings[kCompositeBindings] = {
        fragment_binding(0, rhi::DescriptorKind::SampledTexture),
        fragment_binding(1, rhi::DescriptorKind::SampledTexture),
        fragment_binding(2, rhi::DescriptorKind::SampledTexture),
        fragment_binding(3, rhi::DescriptorKind::StorageBuffer),
    };
    rhi::DescriptorSetLayoutDescription set;
    set.name = "cy selection outline";
    set.bindings = Span<const rhi::DescriptorBinding>(bindings, kCompositeBindings);
    Expected<rhi::DescriptorSetLayoutHandle, Error> made =
        device_->create_descriptor_set_layout(set);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    composite_set_ = *made;

    const rhi::PushConstantRange range{rhi::ShaderStage::Fragment, 0,
                                       static_cast<u32>(sizeof(OutlineConstants))};
    rhi::PipelineLayoutDescription layout;
    layout.name = "cy selection outline layout";
    layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(&composite_set_, 1);
    layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> created = device_->create_pipeline_layout(layout);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    composite_layout_ = *created;

    // PREMULTIPLIED, as the shader writes it: `One, OneMinusSourceAlpha` puts a solid outline's
    // bytes into the output exactly and blends a glow over what is there.
    rhi::ColorAttachmentState color;
    color.format = output_format_;
    color.blend_enable = true;
    color.source_color = rhi::BlendFactor::One;
    color.destination_color = rhi::BlendFactor::OneMinusSourceAlpha;
    color.source_alpha = rhi::BlendFactor::One;
    color.destination_alpha = rhi::BlendFactor::OneMinusSourceAlpha;
    rhi::GraphicsPipelineDescription description;
    description.name = "cy selection outline";
    description.layout = composite_layout_;
    description.vertex_shader = composite_vertex_;
    description.fragment_shader = composite_fragment_;
    description.color_attachments = Span<const rhi::ColorAttachmentState>(&color, 1);
    description.rasterisation.cull_mode = rhi::CullMode::None;
    description.depth_stencil.format = rhi::Format::Undefined;
    Expected<rhi::GraphicsPipelineHandle, Error> pipeline =
        device_->create_graphics_pipeline(description);
    if (!pipeline.has_value()) {
        return make_unexpected(pipeline.error());
    }
    composite_pipeline_ = *pipeline;
    return ok();
}

Status OutlinePass::create_buffers() noexcept {
    style_slots_ = device_->frames_in_flight();
    if (style_slots_ == 0 || style_slots_ > rhi::kMaxFramesInFlight) {
        return fail(ErrorCode::InvalidArgument,
                    "selection outlines: the device reported an impossible frames-in-flight");
    }
    for (u32 index = 0; index < style_slots_; ++index) {
        rhi::BufferDescription buffer;
        buffer.name = "cy selection outline styles";
        buffer.size = kStyleBytes;
        buffer.usage = rhi::BufferUsage::Storage;
        buffer.memory = rhi::MemoryUse::Upload;
        Expected<rhi::BufferHandle, Error> made = device_->create_buffer(buffer);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        styles_[index] = *made;
    }
    if (!desc_.readback) {
        return ok();
    }
    for (rhi::BufferHandle& readback : readbacks_) {
        rhi::BufferDescription host;
        host.name = "cy selection outline readback";
        host.size = image_bytes(desc_);
        host.usage = rhi::BufferUsage::TransferDestination;
        host.memory = rhi::MemoryUse::Readback;
        Expected<rhi::BufferHandle, Error> made = device_->create_buffer(host);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        readback = *made;
    }
    return ok();
}

void OutlinePass::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    rhi::Device& device = *device_;
    for (rhi::BufferHandle& buffer : styles_) {
        if (!buffer.is_null()) {
            device.destroy_buffer(buffer);
            buffer = rhi::BufferHandle{};
        }
    }
    for (rhi::BufferHandle& buffer : readbacks_) {
        if (!buffer.is_null()) {
            device.destroy_buffer(buffer);
            buffer = rhi::BufferHandle{};
        }
    }
    for (rhi::GraphicsPipelineHandle* pipeline : {&mask_pipeline_, &composite_pipeline_}) {
        if (!pipeline->is_null()) {
            device.destroy_graphics_pipeline(*pipeline);
            *pipeline = rhi::GraphicsPipelineHandle{};
        }
    }
    for (rhi::PipelineLayoutHandle* layout : {&mask_layout_, &composite_layout_}) {
        if (!layout->is_null()) {
            device.destroy_pipeline_layout(*layout);
            *layout = rhi::PipelineLayoutHandle{};
        }
    }
    if (!composite_set_.is_null()) {
        device.destroy_descriptor_set_layout(composite_set_);
        composite_set_ = rhi::DescriptorSetLayoutHandle{};
    }
    for (rhi::ShaderModuleHandle* module :
         {&mask_vertex_, &mask_fragment_, &composite_vertex_, &composite_fragment_}) {
        if (!module->is_null()) {
            device.destroy_shader_module(*module);
            *module = rhi::ShaderModuleHandle{};
        }
    }
    style_slots_ = 0;
    highlights_ = nullptr;
    recorder_ = nullptr;
    device_ = nullptr;
}

Status OutlinePass::set_highlights(const HighlightSet* highlights,
                                   const OutlineSettings& settings) noexcept {
    if (Status valid = validate(settings); !valid) {
        return valid;
    }
    highlights_ = highlights;
    settings_ = settings;
    return ok();
}

FrameStageDeclaration OutlinePass::stage(const pipeline::FrameRecorder& recorder) noexcept {
    recorder_ = &recorder;
    return FrameStageDeclaration{&declare_stage, this};
}

PassId OutlinePass::declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                  void* user) noexcept {
    return static_cast<OutlinePass*>(user)->declare(graph, inputs);
}

Status OutlinePass::upload_styles() noexcept {
    style_ring_ = device_->frame_slot() % style_slots_;
    auto* mapped =
        static_cast<GpuOutlineStyle*>(device_->buffer_mapped_pointer(styles_[style_ring_]));
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "selection outlines: the style buffer is not mapped");
    }
    for (u32 index = 0; index < kMaxHighlightStyles; ++index) {
        mapped[index] = GpuOutlineStyle{};
    }
    if (highlights_ != nullptr) {
        highlights_->write_styles(settings_, Span<GpuOutlineStyle>(mapped, kMaxHighlightStyles));
    }
    return ok();
}

PassId OutlinePass::declare(RenderGraph& graph, const ScreenSpaceStageInputs& inputs) noexcept {
    // Each refusal is a frame that would otherwise outline against the wrong image.
    if (device_ == nullptr || recorder_ == nullptr || inputs.target == kInvalidResource ||
        inputs.depth == kInvalidResource || inputs.width != desc_.width ||
        inputs.height != desc_.height) {
        return kInvalidPass;
    }
    if (!upload_styles()) {
        return kInvalidPass;
    }
    report_ = OutlineReport{};
    if (highlights_ != nullptr) {
        constants_ = make_outline_constants(*highlights_, settings_, desc_.width, desc_.height);
    } else {
        constants_ = OutlineConstants{};
    }

    TextureRequest request;
    request.width = desc_.width;
    request.height = desc_.height;
    request.name = "selection mask";
    request.format = rhi::Format::R32Uint;
    mask_ = graph.create_texture(request);
    request.name = "selection mask depth";
    request.format = depth_format_;
    mask_depth_ = graph.create_texture(request);
    scene_depth_ = inputs.depth;
    target_ = inputs.target;

    PassBuilder mask = graph.add_pass("selection mask", QueueKind::Graphics);
    mask.write(mask_, Access::ColorAttachmentWrite)
        .write(mask_depth_, Access::DepthStencilAttachmentWrite);
    if (inputs.draw_instances != kInvalidResource) {
        mask.read(inputs.draw_instances, Access::VertexStorageRead);
    }
    mask.record(&record_mask_fn, this);
    const PassId first = mask.id();

    graph.add_pass("selection outline", QueueKind::Graphics)
        .read(mask_, Access::FragmentSampledRead)
        .read(mask_depth_, Access::FragmentSampledRead)
        .read(scene_depth_, Access::FragmentSampledRead)
        .use(target_, Access::ColorAttachmentReadWrite)
        .record(&record_composite_fn, this);

    if (desc_.readback) {
        const ResourceId sources[kReadbackCount] = {mask_, mask_depth_, scene_depth_};
        ResourceId destinations[kReadbackCount] = {};
        for (u32 index = 0; index < kReadbackCount; ++index) {
            BufferRequest buffer;
            buffer.name = "selection outline readback";
            buffer.size = image_bytes(desc_);
            buffer.extra_usage = rhi::BufferUsage::TransferDestination;
            destinations[index] = graph.import_buffer(buffer, readbacks_[index]);
        }
        PassBuilder copy = graph.add_pass("selection outline readback", QueueKind::Graphics);
        for (u32 index = 0; index < kReadbackCount; ++index) {
            copy.read(sources[index], Access::TransferRead)
                .write(destinations[index], Access::TransferWrite);
        }
        copy.record(&record_readback_fn, this);
        // The host boundary as a declared read, so the graph derives the transfer-to-host barrier.
        PassBuilder host = graph.add_pass("selection outline host", QueueKind::Graphics);
        for (const ResourceId destination : destinations) {
            host.read(destination, Access::HostRead);
        }
        host.side_effect();
    }
    return first;
}

void OutlinePass::record_mask(const PassContext& context) noexcept {
    const GraphExecutor& executor = *context.executor;
    rhi::RenderAttachment color;
    color.view = executor.view(mask_);
    // Zero is "nothing marked", as an unsigned word; the float clear's bits are the same zeros.
    color.load = rhi::LoadOp::Clear;
    color.store = rhi::StoreOp::Store;
    rhi::RenderAttachment depth;
    depth.view = executor.view(mask_depth_);
    depth.load = rhi::LoadOp::Clear;
    depth.store = rhi::StoreOp::Store;
    depth.clear = rhi::reversed_z_depth_clear();
    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, desc_.width, desc_.height};
    info.color_attachments = Span<const rhi::RenderAttachment>(&color, 1);
    info.depth_attachment = depth;

    rhi::CommandBuffer& commands = *context.commands;
    commands.begin_rendering(info);
    set_full_viewport(commands, desc_.width, desc_.height);
    draw_marked(commands);
    commands.end_rendering();
}

void OutlinePass::draw_marked(rhi::CommandBuffer& commands) noexcept {
    const pipeline::FrameRecorder& recorder = *recorder_;
    const pipeline::FrameAssembly* assembly = recorder.assembly();
    const pipeline::GeometrySource& geometry = recorder.geometry();
    if (highlights_ == nullptr || highlights_->empty() || assembly == nullptr ||
        recorder.bindings() == nullptr || geometry.geometry == nullptr ||
        geometry.streams[pipeline::kPositionStream].is_null()) {
        return;
    }
    // The opaque layer is what the prepass drew, so it is what "hidden" is measured against.
    const auto& list = assembly->draws();
    const Span<const render::DrawItem> opaque = assembly->layer(render::SortLayer::Opaque);
    if (opaque.empty()) {
        return;
    }

    commands.bind_graphics_pipeline(mask_pipeline_);
    const Span<const rhi::DescriptorSetHandle> sets = recorder.bindings()->sets();
    commands.bind_descriptor_sets(
        mask_layout_, 0,
        Span<const rhi::DescriptorSetHandle>(sets.data(), sets.size() < 2 ? sets.size() : 2));
    const u64 offset = 0;
    commands.bind_vertex_buffers(
        0, Span<const rhi::BufferHandle>(&geometry.streams[pipeline::kPositionStream], 1),
        Span<const u64>(&offset, 1));

    const auto first = static_cast<u32>(opaque.data() - list.items.data());
    rhi::BufferHandle bound_indices;
    for (u32 offset_in_layer = 0; offset_in_layer < opaque.size(); ++offset_in_layer) {
        const u32 index = first + offset_in_layer;
        const u32 slot = highlights_->slot_of(list.items[index].stable_id);
        if (slot == 0) {
            continue;
        }
        pipeline::DrawGeometry draw;
        if (index >= kDrawIndexLimit ||
            !geometry.geometry(list.items[index], list.instances[index], geometry.user, draw)) {
            ++report_.skipped_draws;
            continue;
        }
        const u32 word = index | (slot << kSlotShift);
        commands.push_constants(mask_layout_, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment,
                                0,
                                Span<const u8>(reinterpret_cast<const u8*>(&word), sizeof(word)));
        if (draw.indices.is_null()) {
            commands.draw(draw.vertex_count, 1, 0, 0);
        } else {
            if (!(draw.indices == bound_indices)) {
                commands.bind_index_buffer(draw.indices, 0, draw.wide_indices);
                bound_indices = draw.indices;
            }
            commands.draw_indexed(draw.index_count, 1, draw.first_index, draw.vertex_offset, 0);
        }
        ++report_.marked_draws;
    }
}

void OutlinePass::record_composite(const PassContext& context) noexcept {
    // NOTHING MARKED WAS DRAWN, SO NOTHING IS COMPOSITED: the output keeps the post chain's bytes.
    if (report_.marked_draws == 0) {
        return;
    }
    const GraphExecutor& executor = *context.executor;
    Expected<rhi::DescriptorSetHandle, Error> set =
        device_->allocate_descriptor_set(composite_set_, true);
    if (!set.has_value()) {
        return;
    }
    rhi::DescriptorWrite styles;
    styles.binding = 3;
    styles.kind = rhi::DescriptorKind::StorageBuffer;
    styles.buffer = styles_[style_ring_];
    const rhi::DescriptorWrite writes[kCompositeBindings] = {
        sampled(0, executor.view(mask_)),
        sampled(1, executor.view(mask_depth_)),
        sampled(2, executor.view(scene_depth_)),
        styles,
    };
    if (Status written = device_->update_descriptor_set(
            *set, Span<const rhi::DescriptorWrite>(writes, kCompositeBindings));
        !written) {
        return;
    }

    rhi::RenderAttachment target;
    target.view = executor.view(target_);
    target.load = rhi::LoadOp::Load;
    target.store = rhi::StoreOp::Store;
    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, desc_.width, desc_.height};
    info.color_attachments = Span<const rhi::RenderAttachment>(&target, 1);

    rhi::CommandBuffer& commands = *context.commands;
    commands.begin_rendering(info);
    set_full_viewport(commands, desc_.width, desc_.height);
    commands.bind_graphics_pipeline(composite_pipeline_);
    commands.bind_descriptor_sets(composite_layout_, 0,
                                  Span<const rhi::DescriptorSetHandle>(&*set, 1));
    commands.push_constants(
        composite_layout_, rhi::ShaderStage::Fragment, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&constants_), sizeof(OutlineConstants)));
    commands.draw(3, 1, 0, 0);
    commands.end_rendering();
    report_.composited = true;
}

void OutlinePass::record_readback(const PassContext& context) noexcept {
    const ResourceId sources[kReadbackCount] = {mask_, mask_depth_, scene_depth_};
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{desc_.width, desc_.height, 1};
    for (u32 index = 0; index < kReadbackCount; ++index) {
        context.commands->copy_texture_to_buffer(context.executor->texture(sources[index]),
                                                 readbacks_[index],
                                                 Span<const rhi::BufferTextureCopy>(&region, 1));
    }
}

Status OutlinePass::read_back(const OutlineReadback& out) const noexcept {
    if (device_ == nullptr || readbacks_[kReadbackMask].is_null()) {
        return fail(ErrorCode::Unavailable,
                    "selection outlines: created without `readback`, so there is nothing to read");
    }
    const usize pixels = static_cast<usize>(desc_.width) * desc_.height;
    if (out.mask.size() != pixels || out.mask_depth.size() != pixels ||
        out.scene_depth.size() != pixels) {
        return fail(ErrorCode::InvalidArgument,
                    "selection outlines: a readback span is not the view");
    }
    const void* sources[kReadbackCount] = {
        device_->buffer_mapped_pointer(readbacks_[kReadbackMask]),
        device_->buffer_mapped_pointer(readbacks_[kReadbackMaskDepth]),
        device_->buffer_mapped_pointer(readbacks_[kReadbackSceneDepth]),
    };
    for (const void* source : sources) {
        if (source == nullptr) {
            return fail(ErrorCode::Internal, "selection outlines: a readback is not mapped");
        }
    }
    std::memcpy(out.mask.data(), sources[kReadbackMask], pixels * sizeof(u32));
    std::memcpy(out.mask_depth.data(), sources[kReadbackMaskDepth], pixels * sizeof(f32));
    std::memcpy(out.scene_depth.data(), sources[kReadbackSceneDepth], pixels * sizeof(f32));
    return ok();
}

}  // namespace cy::rendering::selection
