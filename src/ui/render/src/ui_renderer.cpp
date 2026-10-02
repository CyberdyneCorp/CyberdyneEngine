// SPDX-License-Identifier: MIT
#include <cy/ui/render/ui_renderer.h>

#include <cy/backends/rhi/command_buffer.h>
#include <cy/backends/rhi/validation.h>
#include <cy/rendering/graph/executor.h>

#include "ui_msl.h"
#include "ui_spirv.h"

#include <cstring>

namespace cy::ui::render {
namespace {

using rendering::GraphExecutor;
using rendering::PassContext;
using rendering::PassId;
using rendering::RenderGraph;
using rendering::ResourceId;
using rendering::ScreenSpaceStageInputs;
using rhi::Access;
using rhi::QueueKind;

/// ui.slang's set: primitives, atlas page, point sampler, linear sampler — the `ParameterBlock`'s
/// field order, which is also the argument-buffer id Metal assigns.
constexpr u32 kBindings = 4;
constexpr u32 kQuadIndices = 6;

/// `VkDrawIndexedIndirectCommand`, which every backend's indexed indirect draw reads.
struct IndirectArguments {
    u32 index_count = 0;
    u32 instance_count = 0;
    u32 first_index = 0;
    i32 vertex_offset = 0;
    u32 first_instance = 0;
};
static_assert(sizeof(IndirectArguments) == 20, "an indexed indirect draw is five words");

template <typename T, usize N>
[[nodiscard]] Span<const u32> words(const T (&module)[N]) noexcept {
    return Span<const u32>(module, N);
}

template <usize N>
[[nodiscard]] Span<const u8> text(const char (&module)[N]) noexcept {
    return {reinterpret_cast<const u8*>(module), N - 1U};
}

[[nodiscard]] Expected<rhi::ShaderModuleHandle, Error> create_module(
    rhi::Device& device, const char* name, rhi::ShaderStage stage, Span<const u32> spirv,
    Span<const u8> msl, const char* entry) noexcept {
    rhi::ShaderModuleBundle bundle;
    bundle.spirv = spirv;
    bundle.msl = msl;
    bundle.msl_entry_point = entry;
    bundle.spirv_entry_point = "main";
    rhi::ValidationMessage message;
    Expected<rhi::ShaderModuleDescription, Error> module = rhi::select_shader_module(
        bundle, device.capabilities().native_shader_format(), name, stage, message);
    if (!module.has_value()) {
        return make_unexpected(module.error());
    }
    return device.create_shader_module(*module);
}

[[nodiscard]] rhi::DescriptorBinding binding(u32 index, rhi::DescriptorKind kind,
                                             rhi::ShaderStage stages) noexcept {
    rhi::DescriptorBinding entry;
    entry.binding = index;
    entry.kind = kind;
    entry.count = 1;
    entry.stages = stages;
    return entry;
}

[[nodiscard]] bool is_srgb(rhi::Format format) noexcept {
    return format == rhi::Format::Rgba8Srgb || format == rhi::Format::Bgra8Srgb;
}

[[nodiscard]] u32 bytes_per_texel(rhi::Format format) noexcept {
    if (format == rhi::Format::R8Unorm) {
        return 1U;
    }
    if (format == rhi::Format::Rgba8Unorm) {
        return 4U;
    }
    return 0U;
}

struct AtlasUpload {
    rhi::BufferHandle staging;
    rhi::TextureHandle texture;
    u32 width = 0;
    u32 height = 0;
};

void record_atlas_upload(const PassContext& context, void* user) noexcept {
    const auto* upload = static_cast<const AtlasUpload*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{upload->width, upload->height, 1};
    context.commands->copy_buffer_to_texture(upload->staging, upload->texture,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

/// Copy a page's texels in through a graph of its own and leave it in the sampled layout the pass
/// reads it in — the graph derives both transitions, as grading's table upload does.
[[nodiscard]] Status run_atlas_upload(rhi::Device& device, Allocator& allocator,
                                      AtlasUpload& upload, rhi::Format format) noexcept {
    RenderGraph graph(allocator);
    rendering::TextureRequest request;
    request.name = "ui atlas page";
    request.format = format;
    request.width = upload.width;
    request.height = upload.height;
    const ResourceId page = graph.import_texture(request, upload.texture, rhi::ImageUse::Undefined);
    graph.add_pass("ui atlas upload", QueueKind::Graphics)
        .write(page, Access::TransferWrite)
        .record(&record_atlas_upload, &upload);
    graph.add_pass("ui atlas residency", QueueKind::Graphics)
        .read(page, Access::FragmentSampledRead)
        .side_effect();
    if (Status declared = graph.status(); !declared) {
        return declared;
    }
    const Expected<u32, Error> began = device.begin_frame();
    if (!began.has_value()) {
        return make_unexpected(began.error());
    }
    Status executed = ok();
    {
        GraphExecutor executor(allocator, device);
        if (auto result =
                executor.execute(graph, rendering::CompileOptions{}, rendering::ExecuteOptions{});
            !result) {
            executed = make_unexpected(result.error());
        } else {
            executed = device.wait_idle();
        }
        executor.release();
    }
    if (Status ended = device.end_frame(); !ended && executed) {
        executed = ended;
    }
    return executed;
}

void record_fn(const PassContext& context, void* user) noexcept {
    static_cast<UiRenderer*>(user)->record(context);
}

}  // namespace

UiRenderer::UiRenderer(Allocator& allocator) noexcept : allocator_(&allocator), list_(allocator) {}

UiRenderer::~UiRenderer() {
    destroy();
}

bool UiRenderer::supported(const rhi::Device& device) noexcept {
    const rhi::ShaderFormat format = device.capabilities().native_shader_format();
    return format == rhi::ShaderFormat::Spirv || format == rhi::ShaderFormat::Msl;
}

Status UiRenderer::create(rhi::Device& device, const UiRendererDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "ui renderer: already created");
    }
    if (!supported(device)) {
        return fail(ErrorCode::Unsupported,
                    "ui renderer: this device takes no shader format this module ships (SPIR-V or "
                    "MSL)");
    }
    if (desc.width == 0 || desc.height == 0 || desc.max_primitives == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "UiRendererDescription::width, ::height and ::max_primitives must be non-zero");
    }
    device_ = &device;
    desc_ = desc;
    flags_ = is_srgb(desc.output_format) ? kUiOutputLinear : 0U;
    Status made = create_pipeline();
    if (made) {
        made = create_buffers();
    }
    if (made) {
        // PAGE ZERO IS ONE WHITE TEXEL, so a primitive naming no page samples white and the set
        // the shader declares always has a texture in it.
        const u8 white[4] = {255, 255, 255, 255};
        made = upload_atlas(0, rhi::Format::Rgba8Unorm, 1, 1, Span<const u8>(white, 4));
    }
    if (!made) {
        destroy();
    }
    return made;
}

Status UiRenderer::create_pipeline() noexcept {
    Expected<rhi::ShaderModuleHandle, Error> module =
        create_module(*device_, "cy ui vertex", rhi::ShaderStage::Vertex, words(kUiVertexSpirv),
                      text(kUiVertexMsl), "cyUiVertex");
    if (!module.has_value()) {
        return make_unexpected(module.error());
    }
    vertex_ = *module;
    module = create_module(*device_, "cy ui fragment", rhi::ShaderStage::Fragment,
                           words(kUiFragmentSpirv), text(kUiFragmentMsl), "cyUiFragment");
    if (!module.has_value()) {
        return make_unexpected(module.error());
    }
    fragment_ = *module;

    const rhi::ShaderStage both = rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment;
    const rhi::DescriptorBinding bindings[kBindings] = {
        binding(0, rhi::DescriptorKind::StorageBuffer, both),
        binding(1, rhi::DescriptorKind::SampledTexture, rhi::ShaderStage::Fragment),
        binding(2, rhi::DescriptorKind::Sampler, rhi::ShaderStage::Fragment),
        binding(3, rhi::DescriptorKind::Sampler, rhi::ShaderStage::Fragment),
    };
    rhi::DescriptorSetLayoutDescription set;
    set.name = "cy ui";
    set.bindings = Span<const rhi::DescriptorBinding>(bindings, kBindings);
    Expected<rhi::DescriptorSetLayoutHandle, Error> made =
        device_->create_descriptor_set_layout(set);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    set_layout_ = *made;

    const rhi::PushConstantRange range{both, 0, static_cast<u32>(sizeof(UiPush))};
    rhi::PipelineLayoutDescription layout;
    layout.name = "cy ui layout";
    layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(&set_layout_, 1);
    layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> created = device_->create_pipeline_layout(layout);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    layout_ = *created;

    // PREMULTIPLIED, as the shader writes it: `One, OneMinusSourceAlpha` puts an opaque colour's
    // bytes into the output exactly and blends a translucent one over what is there.
    rhi::ColorAttachmentState color;
    color.format = desc_.output_format;
    color.blend_enable = true;
    color.source_color = rhi::BlendFactor::One;
    color.destination_color = rhi::BlendFactor::OneMinusSourceAlpha;
    color.source_alpha = rhi::BlendFactor::One;
    color.destination_alpha = rhi::BlendFactor::OneMinusSourceAlpha;
    rhi::GraphicsPipelineDescription description;
    description.name = "cy ui";
    description.layout = layout_;
    description.vertex_shader = vertex_;
    description.fragment_shader = fragment_;
    description.color_attachments = Span<const rhi::ColorAttachmentState>(&color, 1);
    description.rasterisation.cull_mode = rhi::CullMode::None;
    description.depth_stencil.format = rhi::Format::Undefined;
    Expected<rhi::GraphicsPipelineHandle, Error> pipeline =
        device_->create_graphics_pipeline(description);
    if (!pipeline.has_value()) {
        return make_unexpected(pipeline.error());
    }
    pipeline_ = *pipeline;

    rhi::SamplerDescription sampler;
    sampler.name = "cy ui point clamp";
    sampler.min_filter = rhi::Filter::Nearest;
    sampler.mag_filter = rhi::Filter::Nearest;
    sampler.mipmap_mode = rhi::MipmapMode::Nearest;
    sampler.address_u = rhi::AddressMode::ClampToEdge;
    sampler.address_v = rhi::AddressMode::ClampToEdge;
    sampler.address_w = rhi::AddressMode::ClampToEdge;
    Expected<rhi::SamplerHandle, Error> point = device_->create_sampler(sampler);
    if (!point.has_value()) {
        return make_unexpected(point.error());
    }
    point_sampler_ = *point;
    sampler.name = "cy ui linear clamp";
    sampler.min_filter = rhi::Filter::Linear;
    sampler.mag_filter = rhi::Filter::Linear;
    Expected<rhi::SamplerHandle, Error> linear = device_->create_sampler(sampler);
    if (!linear.has_value()) {
        return make_unexpected(linear.error());
    }
    linear_sampler_ = *linear;
    return ok();
}

Status UiRenderer::create_buffers() noexcept {
    slots_ = device_->frames_in_flight();
    if (slots_ == 0 || slots_ > rhi::kMaxFramesInFlight) {
        return fail(ErrorCode::InvalidArgument,
                    "ui renderer: the device reported an impossible frames-in-flight");
    }
    rhi::BufferDescription indices;
    indices.name = "cy ui quad indices";
    indices.size = kQuadIndices * sizeof(u16);
    indices.usage = rhi::BufferUsage::Index;
    indices.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> made = device_->create_buffer(indices);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    indices_ = *made;
    auto* mapped = static_cast<u16*>(device_->buffer_mapped_pointer(indices_));
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "ui renderer: the index buffer is not mapped");
    }
    // Two triangles over corners 0 (top-left), 1 (top-right), 2 (bottom-left), 3 (bottom-right).
    const u16 quad[kQuadIndices] = {0, 1, 2, 2, 1, 3};
    std::memcpy(mapped, quad, sizeof(quad));

    for (u32 index = 0; index < slots_; ++index) {
        rhi::BufferDescription rows;
        rows.name = "cy ui primitives";
        rows.size = static_cast<u64>(desc_.max_primitives) * sizeof(GpuUiPrimitive);
        rows.usage = rhi::BufferUsage::Storage;
        rows.memory = rhi::MemoryUse::Upload;
        made = device_->create_buffer(rows);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        rows_[index] = *made;
        rhi::BufferDescription arguments;
        arguments.name = "cy ui draw arguments";
        // One draw per batch, and a batch holds at least one primitive.
        arguments.size = static_cast<u64>(desc_.max_primitives) * sizeof(IndirectArguments);
        arguments.usage = rhi::BufferUsage::Indirect;
        arguments.memory = rhi::MemoryUse::Upload;
        made = device_->create_buffer(arguments);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        arguments_[index] = *made;
    }
    return ok();
}

void UiRenderer::destroy_page(AtlasPage& page) noexcept {
    if (!page.view.is_null()) {
        device_->destroy_texture_view(page.view);
        page.view = rhi::TextureViewHandle{};
    }
    if (!page.texture.is_null()) {
        device_->destroy_texture(page.texture);
        page.texture = rhi::TextureHandle{};
    }
}

void UiRenderer::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    rhi::Device& device = *device_;
    for (AtlasPage& page : pages_) {
        destroy_page(page);
    }
    const auto release = [&device](rhi::BufferHandle& buffer) noexcept {
        if (!buffer.is_null()) {
            device.destroy_buffer(buffer);
            buffer = rhi::BufferHandle{};
        }
    };
    for (u32 index = 0; index < rhi::kMaxFramesInFlight; ++index) {
        release(rows_[index]);
        release(arguments_[index]);
    }
    release(indices_);
    for (rhi::SamplerHandle* sampler : {&point_sampler_, &linear_sampler_}) {
        if (!sampler->is_null()) {
            device.destroy_sampler(*sampler);
            *sampler = rhi::SamplerHandle{};
        }
    }
    if (!pipeline_.is_null()) {
        device.destroy_graphics_pipeline(pipeline_);
        pipeline_ = rhi::GraphicsPipelineHandle{};
    }
    if (!layout_.is_null()) {
        device.destroy_pipeline_layout(layout_);
        layout_ = rhi::PipelineLayoutHandle{};
    }
    if (!set_layout_.is_null()) {
        device.destroy_descriptor_set_layout(set_layout_);
        set_layout_ = rhi::DescriptorSetLayoutHandle{};
    }
    for (rhi::ShaderModuleHandle* module : {&vertex_, &fragment_}) {
        if (!module->is_null()) {
            device.destroy_shader_module(*module);
            *module = rhi::ShaderModuleHandle{};
        }
    }
    list_.clear();
    report_ = UiRenderReport{};
    slots_ = 0;
    device_ = nullptr;
}

Status UiRenderer::upload_atlas(u16 page, rhi::Format format, u32 width, u32 height,
                                Span<const u8> pixels) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "ui renderer: not created");
    }
    const u32 texel_bytes = bytes_per_texel(format);
    if (page >= kMaxAtlasPages || texel_bytes == 0 || width == 0 || height == 0 ||
        pixels.size() != static_cast<usize>(width) * height * texel_bytes) {
        return fail(ErrorCode::InvalidArgument,
                    "ui renderer: an atlas page is R8Unorm or Rgba8Unorm, its texels its extent, "
                    "and its number below kMaxAtlasPages");
    }
    rhi::TextureDescription texture;
    texture.name = "ui atlas page";
    texture.format = format;
    texture.extent = rhi::Extent3D{width, height, 1};
    texture.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination;
    Expected<rhi::TextureHandle, Error> image = device_->create_texture(texture);
    if (!image.has_value()) {
        return make_unexpected(image.error());
    }
    rhi::BufferDescription staging;
    staging.name = "ui atlas staging";
    staging.size = pixels.size();
    staging.usage = rhi::BufferUsage::TransferSource;
    staging.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(staging);
    if (!buffer.has_value()) {
        device_->destroy_texture(*image);
        return make_unexpected(buffer.error());
    }
    void* mapped = device_->buffer_mapped_pointer(*buffer);
    Status uploaded =
        mapped != nullptr ? ok() : fail(ErrorCode::Internal, "ui renderer: staging is not mapped");
    if (uploaded) {
        std::memcpy(mapped, pixels.data(), pixels.size());
        AtlasUpload upload{*buffer, *image, width, height};
        uploaded = run_atlas_upload(*device_, *allocator_, upload, format);
    }
    device_->destroy_buffer(*buffer);
    if (!uploaded) {
        device_->destroy_texture(*image);
        return uploaded;
    }
    rhi::TextureViewDescription view;
    view.name = "ui atlas page";
    view.texture = *image;
    Expected<rhi::TextureViewHandle, Error> made = device_->create_texture_view(view);
    if (!made.has_value()) {
        device_->destroy_texture(*image);
        return make_unexpected(made.error());
    }
    // The previous page is idle: the upload above waited for the device.
    destroy_page(pages_[page]);
    pages_[page] = AtlasPage{*image, *made};
    return ok();
}

bool UiRenderer::has_atlas(u16 page) const noexcept {
    return page < kMaxAtlasPages && !pages_[page].view.is_null();
}

Status UiRenderer::submit(const PrimitiveBuffer& buffer, f32 scale) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "ui renderer: not created");
    }
    if (buffer.primitives().size() > desc_.max_primitives) {
        list_.clear();
        return fail(ErrorCode::OutOfMemory,
                    "ui renderer: the stream is larger than UiRendererDescription::max_primitives");
    }
    if (!(scale > 0.0F)) {
        list_.clear();
        return fail(ErrorCode::InvalidArgument, "ui renderer: the scale must be positive");
    }
    return build_draws(buffer, scale, desc_.width, desc_.height, list_);
}

void UiRenderer::clear() noexcept {
    list_.clear();
}

rendering::FrameStageDeclaration UiRenderer::stage() noexcept {
    return rendering::FrameStageDeclaration{&declare_stage, this};
}

PassId UiRenderer::declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                 void* user) noexcept {
    return static_cast<UiRenderer*>(user)->declare(graph, inputs);
}

PassId UiRenderer::declare(RenderGraph& graph, const ScreenSpaceStageInputs& inputs) noexcept {
    // Each refusal is a frame that would otherwise draw an interface laid out for another target.
    if (device_ == nullptr || inputs.target == rendering::kInvalidResource ||
        inputs.width != desc_.width || inputs.height != desc_.height) {
        return rendering::kInvalidPass;
    }
    report_ = UiRenderReport{};
    report_.primitives = static_cast<u32>(list_.rows.size());
    report_.clipped_batches = list_.clipped_batches;

    // This frame's rows and arguments, into this frame slot's buffers: the device finished
    // reading them `frames_in_flight` frames ago.
    slot_ = device_->frame_slot() % slots_;
    auto* rows = static_cast<GpuUiPrimitive*>(device_->buffer_mapped_pointer(rows_[slot_]));
    auto* arguments =
        static_cast<IndirectArguments*>(device_->buffer_mapped_pointer(arguments_[slot_]));
    if (rows == nullptr || arguments == nullptr) {
        return rendering::kInvalidPass;
    }
    if (!list_.rows.empty()) {
        std::memcpy(rows, list_.rows.data(), list_.rows.size() * sizeof(GpuUiPrimitive));
    }
    for (usize index = 0; index < list_.draws.size(); ++index) {
        arguments[index] = IndirectArguments{kQuadIndices, list_.draws[index].count, 0, 0, 0};
    }

    target_ = inputs.target;
    return graph.add_pass("ui", QueueKind::Graphics)
        .use(target_, Access::ColorAttachmentReadWrite)
        .record(&record_fn, this)
        .id();
}

Expected<rhi::DescriptorSetHandle, Error> UiRenderer::set_for(u16 page, u32 slot) noexcept {
    const AtlasPage& bound = has_atlas(page) ? pages_[page] : pages_[0];
    Expected<rhi::DescriptorSetHandle, Error> set =
        device_->allocate_descriptor_set(set_layout_, true);
    if (!set.has_value()) {
        return set;
    }
    rhi::DescriptorWrite writes[kBindings] = {};
    writes[0].binding = 0;
    writes[0].kind = rhi::DescriptorKind::StorageBuffer;
    writes[0].buffer = rows_[slot];
    writes[1].binding = 1;
    writes[1].kind = rhi::DescriptorKind::SampledTexture;
    writes[1].texture_view = bound.view;
    writes[1].use = rhi::ImageUse::SampledRead;
    writes[2].binding = 2;
    writes[2].kind = rhi::DescriptorKind::Sampler;
    writes[2].sampler = point_sampler_;
    writes[3].binding = 3;
    writes[3].kind = rhi::DescriptorKind::Sampler;
    writes[3].sampler = linear_sampler_;
    if (Status written = device_->update_descriptor_set(
            *set, Span<const rhi::DescriptorWrite>(writes, kBindings));
        !written) {
        return make_unexpected(written.error());
    }
    return set;
}

void UiRenderer::record(const PassContext& context) noexcept {
    // NOTHING SUBMITTED, NOTHING RECORDED: the output keeps the chain's bytes.
    if (list_.draws.empty()) {
        return;
    }
    rhi::RenderAttachment target;
    target.view = context.executor->view(target_);
    target.load = rhi::LoadOp::Load;
    target.store = rhi::StoreOp::Store;
    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, desc_.width, desc_.height};
    info.color_attachments = Span<const rhi::RenderAttachment>(&target, 1);

    rhi::CommandBuffer& commands = *context.commands;
    commands.begin_rendering(info);
    commands.set_viewport(rhi::Viewport{0.0F, 0.0F, static_cast<f32>(desc_.width),
                                        static_cast<f32>(desc_.height), 0.0F, 1.0F});
    commands.bind_graphics_pipeline(pipeline_);
    commands.bind_index_buffer(indices_, 0, false);

    UiPush push;
    push.inverse_extent[0] = 2.0F / static_cast<f32>(desc_.width);
    push.inverse_extent[1] = 2.0F / static_cast<f32>(desc_.height);
    push.flags = flags_;
    i32 bound_page = -1;
    for (usize index = 0; index < list_.draws.size(); ++index) {
        const UiDraw& draw = list_.draws[index];
        if (static_cast<i32>(draw.atlas) != bound_page) {
            Expected<rhi::DescriptorSetHandle, Error> set = set_for(draw.atlas, slot_);
            if (!set.has_value()) {
                break;
            }
            commands.bind_descriptor_sets(layout_, 0,
                                          Span<const rhi::DescriptorSetHandle>(&*set, 1));
            bound_page = static_cast<i32>(draw.atlas);
            ++report_.page_binds;
        }
        commands.set_scissor(rhi::Rect2D{static_cast<i32>(draw.scissor.x),
                                         static_cast<i32>(draw.scissor.y), draw.scissor.width,
                                         draw.scissor.height});
        push.first = draw.first;
        commands.push_constants(layout_, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
                                Span<const u8>(reinterpret_cast<const u8*>(&push), sizeof(push)));
        commands.draw_indexed_indirect(arguments_[slot_], index * sizeof(IndirectArguments), 1,
                                       sizeof(IndirectArguments));
        ++report_.draws;
    }
    commands.end_rendering();
    report_.recorded = true;
}

}  // namespace cy::ui::render
