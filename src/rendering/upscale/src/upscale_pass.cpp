// SPDX-License-Identifier: MIT
#include <cy/rendering/upscale/upscale_pass.h>

#include <cy/backends/rhi/validation.h>
#include <cy/rendering/graph/executor.h>

#include "upscale_msl.h"
#include "upscale_spirv.h"

#include <cstring>

namespace cy::rendering::upscale {
namespace {

using rhi::Access;
using rhi::QueueKind;

constexpr u32 kGroupSize = 8;
/// The most bindings a dispatch's set has — the reconstruction's six inputs and its two outputs.
/// Each set is its shader's `ParameterBlock` in field order, which is also the argument buffer id
/// Metal assigns, and the last `storage` bindings of each are the storage images it writes.
constexpr u32 kMaxBindings = 8;

struct ShaderSource {
    const char* name;
    const char* entry;
    Span<const u32> spirv;
    const char* msl;
    usize msl_bytes;
    u32 bindings;
    u32 storage = 1;
};

[[nodiscard]] ShaderSource compute_source(u32 stage) noexcept {
    switch (stage) {
        case 0:
            return {"upscale reconstruct",
                    "cyUpscaleReconstruct",
                    {kUpscaleReconstructSpirv, sizeof(kUpscaleReconstructSpirv) / sizeof(u32)},
                    kUpscaleReconstructMsl,
                    sizeof(kUpscaleReconstructMsl) - 1,
                    8,
                    2};
        case 1:
            return {"upscale sharpen",
                    "cyUpscaleSharpen",
                    {kUpscaleSharpenSpirv, sizeof(kUpscaleSharpenSpirv) / sizeof(u32)},
                    kUpscaleSharpenMsl,
                    sizeof(kUpscaleSharpenMsl) - 1,
                    2};
        default:
            return {"upscale opaque copy",
                    "cyUpscaleCopy",
                    {kUpscaleCopySpirv, sizeof(kUpscaleCopySpirv) / sizeof(u32)},
                    kUpscaleCopyMsl,
                    sizeof(kUpscaleCopyMsl) - 1,
                    2};
    }
}

/// The push constants each dispatch reads: the reconstruction's block, the sharpen's, or the copy's
/// extent.
[[nodiscard]] u32 push_bytes(u32 stage) noexcept {
    switch (stage) {
        case 0:
            return static_cast<u32>(sizeof(UpscaleConstants));
        case 1:
            return static_cast<u32>(sizeof(SharpenConstants));
        default:
            return 16U;
    }
}

[[nodiscard]] u32 groups(u32 extent) noexcept {
    return (extent + kGroupSize - 1U) / kGroupSize;
}

[[nodiscard]] rhi::DescriptorWrite sampled(u32 binding, rhi::TextureViewHandle view) noexcept {
    rhi::DescriptorWrite write;
    write.binding = binding;
    write.kind = rhi::DescriptorKind::SampledTexture;
    write.texture_view = view;
    write.use = rhi::ImageUse::SampledRead;
    return write;
}

[[nodiscard]] rhi::DescriptorWrite storage(u32 binding, rhi::TextureViewHandle view) noexcept {
    rhi::DescriptorWrite write;
    write.binding = binding;
    write.kind = rhi::DescriptorKind::StorageTexture;
    write.texture_view = view;
    write.use = rhi::ImageUse::Storage;
    return write;
}

[[nodiscard]] Expected<rhi::ShaderModuleHandle, Error> make_module(
    rhi::Device& device, const ShaderSource& source, rhi::ShaderStage stage) noexcept {
    rhi::ShaderModuleBundle bundle;
    bundle.spirv = source.spirv;
    bundle.msl = {reinterpret_cast<const u8*>(source.msl), source.msl_bytes};
    bundle.msl_entry_point = source.entry;
    bundle.spirv_entry_point = "main";
    rhi::ValidationMessage message;
    auto module = rhi::select_shader_module(bundle, device.capabilities().native_shader_format(),
                                            source.name, stage, message);
    if (!module.has_value()) {
        return make_unexpected(module.error());
    }
    return device.create_shader_module(*module);
}

/// Bytes a texel of each read-back image is, and its name.
constexpr u64 kCopyTexelBytes[] = {8U, 4U, 4U, 8U, 8U, 8U, 8U, 8U, 8U, 4U, 4U};
constexpr const char* kCopyNames[] = {
    "upscale colour readback",           "upscale depth readback",
    "upscale velocity readback",         "upscale opaque readback",
    "upscale previous history readback", "upscale history readback",
    "upscale previous state readback",   "upscale state readback",
    "upscale output readback",           "upscale output depth readback",
    "upscale output velocity readback"};

/// IEEE 754 binary16 to binary32, for the readback of the half-float images.
[[nodiscard]] f32 half_to_float(u16 half) noexcept {
    const u32 sign = static_cast<u32>(half & 0x8000U) << 16U;
    const u32 exponent = (half >> 10U) & 0x1FU;
    u32 mantissa = half & 0x3FFU;
    u32 bits = 0;
    if (exponent == 0U) {
        if (mantissa == 0U) {
            bits = sign;
        } else {
            u32 shift = 0;
            while ((mantissa & 0x400U) == 0U) {
                mantissa <<= 1U;
                ++shift;
            }
            mantissa &= 0x3FFU;
            bits = sign | ((113U - shift) << 23U) | (mantissa << 13U);
        }
    } else if (exponent == 0x1FU) {
        bits = sign | 0x7F800000U | (mantissa << 13U);
    } else {
        bits = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
    }
    f32 value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void read_colour(const void* mapped, Span<Vec4> out) noexcept {
    const auto* texels = static_cast<const u16*>(mapped);
    for (usize pixel = 0; pixel < out.size(); ++pixel) {
        const u16* texel = texels + (pixel * 4U);
        out[pixel] = Vec4{half_to_float(texel[0]), half_to_float(texel[1]), half_to_float(texel[2]),
                          half_to_float(texel[3])};
    }
}

template <typename T>
void read_floats(const void* mapped, Span<T> out) noexcept {
    if (!out.empty()) {
        std::memcpy(out.data(), mapped, out.size() * sizeof(T));
    }
}

void read_motion(const void* mapped, Span<Vec2> out) noexcept {
    const auto* texels = static_cast<const u16*>(mapped);
    for (usize pixel = 0; pixel < out.size(); ++pixel) {
        out[pixel] =
            Vec2{half_to_float(texels[pixel * 2U]), half_to_float(texels[(pixel * 2U) + 1U])};
    }
}

}  // namespace

const char* upscaler_family_name(UpscalerFamily family) noexcept {
    switch (family) {
        case UpscalerFamily::BuiltIn:
            return "built-in";
        case UpscalerFamily::Fsr:
            return "FSR";
        case UpscalerFamily::Dlss:
            return "DLSS";
        case UpscalerFamily::Xess:
            return "XeSS";
        case UpscalerFamily::MetalFx:
            return "MetalFX";
        case UpscalerFamily::Count:
            break;
    }
    return "unknown";
}

namespace {

PassId declare_upscaler(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                        void* user) noexcept {
    return static_cast<Upscaler*>(user)->declare(graph, inputs);
}

void upscaler_executed(void* user) noexcept {
    static_cast<Upscaler*>(user)->executed();
}

}  // namespace

FrameStageDeclaration stage_of(Upscaler& upscaler) noexcept {
    return FrameStageDeclaration{&declare_upscaler, &upscaler, &upscaler_executed};
}

Status UpscalerRegistry::add(Upscaler& vendor) noexcept {
    const auto index = static_cast<u32>(vendor.family());
    if (vendor.family() == UpscalerFamily::BuiltIn || index >= kUpscalerFamilyCount) {
        return fail(ErrorCode::InvalidArgument,
                    "upscaler registry: a vendor module must name a vendor family");
    }
    if (vendors_[index] != nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "upscaler registry: a module for this family was already added");
    }
    vendors_[index] = &vendor;
    return ok();
}

Upscaler& UpscalerRegistry::select(UpscalerFamily preferred,
                                   const rhi::Device* device) const noexcept {
    const auto index = static_cast<u32>(preferred);
    if (index < kUpscalerFamilyCount && preferred != UpscalerFamily::BuiltIn &&
        vendors_[index] != nullptr && vendors_[index]->available(device)) {
        return *vendors_[index];
    }
    return *built_in_;
}

bool UpscalerRegistry::has(UpscalerFamily family) const noexcept {
    const auto index = static_cast<u32>(family);
    if (family == UpscalerFamily::BuiltIn) {
        return true;
    }
    return index < kUpscalerFamilyCount && vendors_[index] != nullptr;
}

// --- The pass ---------------------------------------------------------------------------------

TemporalUpscalePass::~TemporalUpscalePass() {
    destroy();
}

bool TemporalUpscalePass::supported(const rhi::Device& device) noexcept {
    const rhi::ShaderFormat format = device.capabilities().native_shader_format();
    return device.capabilities().has(rhi::Capability::ComputeShaders) &&
           (format == rhi::ShaderFormat::Spirv || format == rhi::ShaderFormat::Msl);
}

bool TemporalUpscalePass::available(const rhi::Device* device) const noexcept {
    return device == nullptr || supported(*device);
}

Status TemporalUpscalePass::create(Allocator& allocator, rhi::Device& device,
                                   const UpscalePassDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the temporal upscaler is already created");
    }
    if (!supported(device)) {
        return fail(ErrorCode::Unsupported,
                    "the temporal upscaler is compute dispatches and this device has no compute "
                    "stage or no shader format this module ships (SPIR-V or MSL)");
    }
    if (desc.output.width == 0 || desc.output.height == 0) {
        return fail(ErrorCode::InvalidArgument, "UpscalePassDescription::output must be non-empty");
    }
    allocator_ = &allocator;
    device_ = &device;
    desc_ = desc;
    for (u32 stage = 0; stage < kComputeStageCount; ++stage) {
        if (Status created = create_compute(static_cast<Stage>(stage)); !created) {
            destroy();
            return created;
        }
    }
    if (Status created = create_resample(); !created) {
        destroy();
        return created;
    }
    if (Status created = create_resources(); !created) {
        destroy();
        return created;
    }
    return ok();
}

Status TemporalUpscalePass::create_compute(Stage stage) noexcept {
    const ShaderSource source = compute_source(stage);
    Expected<rhi::ShaderModuleHandle, Error> shader =
        make_module(*device_, source, rhi::ShaderStage::Compute);
    if (!shader.has_value()) {
        return make_unexpected(shader.error());
    }
    shaders_[stage] = *shader;

    rhi::DescriptorBinding bindings[kMaxBindings] = {};
    for (u32 index = 0; index < source.bindings; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = index + source.storage >= source.bindings
                                   ? rhi::DescriptorKind::StorageTexture
                                   : rhi::DescriptorKind::SampledTexture;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Compute;
    }
    rhi::DescriptorSetLayoutDescription set_layout;
    set_layout.name = source.name;
    set_layout.bindings = Span<const rhi::DescriptorBinding>(bindings, source.bindings);
    Expected<rhi::DescriptorSetLayoutHandle, Error> layout =
        device_->create_descriptor_set_layout(set_layout);
    if (!layout.has_value()) {
        return make_unexpected(layout.error());
    }
    set_layouts_[stage] = *layout;

    const rhi::PushConstantRange range{rhi::ShaderStage::Compute, 0, push_bytes(stage)};
    rhi::PipelineLayoutDescription pipeline_layout;
    pipeline_layout.name = source.name;
    pipeline_layout.set_layouts =
        Span<const rhi::DescriptorSetLayoutHandle>(&set_layouts_[stage], 1);
    pipeline_layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> made =
        device_->create_pipeline_layout(pipeline_layout);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    pipeline_layouts_[stage] = *made;

    rhi::ComputePipelineDescription pipeline;
    pipeline.name = source.name;
    pipeline.layout = pipeline_layouts_[stage];
    pipeline.shader = shaders_[stage];
    pipeline.workgroup_size[0] = kGroupSize;
    pipeline.workgroup_size[1] = kGroupSize;
    Expected<rhi::ComputePipelineHandle, Error> created =
        device_->create_compute_pipeline(pipeline);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    pipelines_[stage] = *created;
    return ok();
}

Status TemporalUpscalePass::create_resample() noexcept {
    const ShaderSource vertex{
        "upscale resample vertex",
        "cyUpscaleResampleVertex",
        {kUpscaleResampleVertexSpirv, sizeof(kUpscaleResampleVertexSpirv) / sizeof(u32)},
        kUpscaleResampleVertexMsl,
        sizeof(kUpscaleResampleVertexMsl) - 1,
        0};
    const ShaderSource fragment{
        "upscale resample fragment",
        "cyUpscaleResampleFragment",
        {kUpscaleResampleFragmentSpirv, sizeof(kUpscaleResampleFragmentSpirv) / sizeof(u32)},
        kUpscaleResampleFragmentMsl,
        sizeof(kUpscaleResampleFragmentMsl) - 1,
        2};
    Expected<rhi::ShaderModuleHandle, Error> made_vertex =
        make_module(*device_, vertex, rhi::ShaderStage::Vertex);
    if (!made_vertex.has_value()) {
        return make_unexpected(made_vertex.error());
    }
    resample_shaders_[0] = *made_vertex;
    Expected<rhi::ShaderModuleHandle, Error> made_fragment =
        make_module(*device_, fragment, rhi::ShaderStage::Fragment);
    if (!made_fragment.has_value()) {
        return make_unexpected(made_fragment.error());
    }
    resample_shaders_[1] = *made_fragment;

    rhi::DescriptorBinding bindings[2] = {};
    for (u32 index = 0; index < 2U; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = rhi::DescriptorKind::SampledTexture;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Fragment;
    }
    rhi::DescriptorSetLayoutDescription set_layout;
    set_layout.name = "upscale resample";
    set_layout.bindings = Span<const rhi::DescriptorBinding>(bindings, 2);
    Expected<rhi::DescriptorSetLayoutHandle, Error> layout =
        device_->create_descriptor_set_layout(set_layout);
    if (!layout.has_value()) {
        return make_unexpected(layout.error());
    }
    resample_set_layout_ = *layout;

    const rhi::PushConstantRange range{rhi::ShaderStage::Fragment, 0,
                                       static_cast<u32>(sizeof(UpscaleConstants))};
    rhi::PipelineLayoutDescription pipeline_layout;
    pipeline_layout.name = "upscale resample";
    pipeline_layout.set_layouts =
        Span<const rhi::DescriptorSetLayoutHandle>(&resample_set_layout_, 1);
    pipeline_layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> made_layout =
        device_->create_pipeline_layout(pipeline_layout);
    if (!made_layout.has_value()) {
        return make_unexpected(made_layout.error());
    }
    resample_layout_ = *made_layout;

    rhi::ColorAttachmentState color;
    color.format = desc_.velocity_format;
    rhi::GraphicsPipelineDescription description;
    description.name = "upscale resample";
    description.layout = resample_layout_;
    description.vertex_shader = resample_shaders_[0];
    description.fragment_shader = resample_shaders_[1];
    description.color_attachments = Span<const rhi::ColorAttachmentState>(&color, 1);
    description.rasterisation.cull_mode = rhi::CullMode::None;
    // EVERY PIXEL WRITTEN, whatever the cleared value: the depth is the render texel's, not a test.
    description.depth_stencil.format = desc_.depth_format;
    description.depth_stencil.depth_test_enable = true;
    description.depth_stencil.depth_write_enable = true;
    description.depth_stencil.depth_compare = rhi::CompareOp::Always;
    Expected<rhi::GraphicsPipelineHandle, Error> created =
        device_->create_graphics_pipeline(description);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    resample_pipeline_ = *created;
    return ok();
}

Status TemporalUpscalePass::create_resources() noexcept {
    rhi::TextureDescription history;
    history.extent = rhi::Extent3D{desc_.output.width, desc_.output.height, 1};
    history.usage =
        rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage | rhi::TextureUsage::TransferSource;
    for (u32 index = 0; index < 2U; ++index) {
        history.name = "upscale history";
        history.format = rhi::Format::Rgba16Sfloat;
        Expected<rhi::TextureHandle, Error> colour = device_->create_texture(history);
        if (!colour.has_value()) {
            return make_unexpected(colour.error());
        }
        histories_[index] = *colour;
        // RG32F: the lock and the depth. A core storage format on every backend, where a 16-bit
        // pair would need Vulkan's extended storage formats and would not hold a depth.
        history.name = "upscale state";
        history.format = rhi::Format::Rg32Sfloat;
        Expected<rhi::TextureHandle, Error> state = device_->create_texture(history);
        if (!state.has_value()) {
            return make_unexpected(state.error());
        }
        states_[index] = *state;
    }
    if (!desc_.readback) {
        return ok();
    }
    const u64 pixels = static_cast<u64>(desc_.output.width) * desc_.output.height;
    for (u32 copy = 0; copy < kCopyCount; ++copy) {
        rhi::BufferDescription host;
        host.size = pixels * kCopyTexelBytes[copy];
        host.usage = rhi::BufferUsage::TransferDestination;
        host.memory = rhi::MemoryUse::Readback;
        host.name = kCopyNames[copy];
        Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(host);
        if (!buffer.has_value()) {
            return make_unexpected(buffer.error());
        }
        readbacks_[copy] = *buffer;
    }
    return ok();
}

void TemporalUpscalePass::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    for (u32 index = 0; index < 2U; ++index) {
        if (!histories_[index].is_null()) {
            device_->destroy_texture(histories_[index]);
        }
        if (!states_[index].is_null()) {
            device_->destroy_texture(states_[index]);
        }
        histories_[index] = {};
        states_[index] = {};
        history_uses_[index] = rhi::ImageUse::Undefined;
        state_uses_[index] = rhi::ImageUse::Undefined;
    }
    for (rhi::BufferHandle& buffer : readbacks_) {
        if (!buffer.is_null()) {
            device_->destroy_buffer(buffer);
        }
        buffer = {};
    }
    for (u32 stage = 0; stage < kComputeStageCount; ++stage) {
        if (!pipelines_[stage].is_null()) {
            device_->destroy_compute_pipeline(pipelines_[stage]);
        }
        if (!pipeline_layouts_[stage].is_null()) {
            device_->destroy_pipeline_layout(pipeline_layouts_[stage]);
        }
        if (!set_layouts_[stage].is_null()) {
            device_->destroy_descriptor_set_layout(set_layouts_[stage]);
        }
        if (!shaders_[stage].is_null()) {
            device_->destroy_shader_module(shaders_[stage]);
        }
        pipelines_[stage] = {};
        pipeline_layouts_[stage] = {};
        set_layouts_[stage] = {};
        shaders_[stage] = {};
    }
    if (!resample_pipeline_.is_null()) {
        device_->destroy_graphics_pipeline(resample_pipeline_);
    }
    if (!resample_layout_.is_null()) {
        device_->destroy_pipeline_layout(resample_layout_);
    }
    if (!resample_set_layout_.is_null()) {
        device_->destroy_descriptor_set_layout(resample_set_layout_);
    }
    for (rhi::ShaderModuleHandle& shader : resample_shaders_) {
        if (!shader.is_null()) {
            device_->destroy_shader_module(shader);
        }
        shader = {};
    }
    resample_pipeline_ = {};
    resample_layout_ = {};
    resample_set_layout_ = {};
    read_ = 0;
    written_ = false;
    declared_ = false;
    device_ = nullptr;
    allocator_ = nullptr;
}

Status TemporalUpscalePass::bind_temporal(TemporalFramework& framework) noexcept {
    if (framework_ != nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "the temporal upscaler is already bound to a temporal framework");
    }
    const Expected<ConsumerId, Error> consumer =
        framework.register_consumer("post.temporal_upscaling", true);
    if (!consumer.has_value()) {
        return make_unexpected(consumer.error());
    }
    // The framework's own accounting: the colour and the state, both at the output extent — the
    // state's depth is the surface each output texel holds, kept where the reconstruction keeps it.
    HistoryDeclaration colour;
    colour.format = HistoryFormat::Rgba16F;
    colour.resolution_scale = 1.0F;
    colour.frames = 2;
    const Expected<HistoryId, Error> colour_history = framework.declare_history(*consumer, colour);
    if (!colour_history.has_value()) {
        return make_unexpected(colour_history.error());
    }
    HistoryDeclaration state;
    state.format = HistoryFormat::Rg32F;
    state.resolution_scale = 1.0F;
    state.frames = 2;
    const Expected<HistoryId, Error> state_history = framework.declare_history(*consumer, state);
    if (!state_history.has_value()) {
        return make_unexpected(state_history.error());
    }
    framework_ = &framework;
    consumer_ = *consumer;
    colour_history_ = *colour_history;
    state_history_ = *state_history;
    return ok();
}

FrameStageDeclaration TemporalUpscalePass::opaque_copy_stage() noexcept {
    return FrameStageDeclaration{&declare_copy_stage, this};
}

PassId TemporalUpscalePass::declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                          void* user) noexcept {
    return static_cast<TemporalUpscalePass*>(user)->declare(graph, inputs);
}

PassId TemporalUpscalePass::declare_copy_stage(RenderGraph& graph,
                                               const ScreenSpaceStageInputs& inputs,
                                               void* user) noexcept {
    auto& self = *static_cast<TemporalUpscalePass*>(user);
    if (self.device_ == nullptr || inputs.source == kInvalidResource ||
        inputs.target == kInvalidResource || inputs.width == 0 || inputs.height == 0) {
        return kInvalidPass;
    }
    Step& step = self.steps_[kCopy];
    step = Step{};
    step.self = &self;
    step.stage = kCopy;
    step.color = inputs.source;
    step.output = inputs.target;
    step.width = inputs.width;
    step.height = inputs.height;
    return graph.add_pass("upscale opaque copy", QueueKind::Graphics)
        .read(inputs.source, Access::ComputeSampledRead)
        .write(inputs.target, Access::ComputeStorageWrite)
        .record(&record_step, &step)
        .id();
}

Status TemporalUpscalePass::prepare(const ScreenSpaceStageInputs& inputs) noexcept {
    const TemporalView& view = framework_->view();
    const HistoryResource* colour = framework_->history(colour_history_);
    UpscaleView upscale;
    upscale.render = Extent2D{inputs.render_width, inputs.render_height};
    upscale.output = Extent2D{inputs.width, inputs.height};
    upscale.jitter = framework_->jitter().current();
    upscale.projection = view.projection;
    upscale.relative_to_clip = camera_relative_to_clip(view);
    upscale.previous_relative_to_clip = previous_camera_relative_to_clip(*framework_);
    // HISTORY IS THE FRAMEWORK'S TO INVALIDATE: a cut, a teleport, a projection or output change
    // reaches this pass as `valid == false` in the frame it happens, and nothing here decides it.
    upscale.history_valid =
        written_ && colour != nullptr && colour->valid && !framework_->invalidated_this_frame();
    upscale.velocity = inputs.velocity != kInvalidResource;
    upscale.reactive = inputs.opaque_color != kInvalidResource;
    Expected<UpscaleConstants, Error> constants = make_upscale_constants(settings_, upscale);
    if (!constants.has_value()) {
        return make_unexpected(constants.error());
    }
    constants_ = *constants;
    sharpen_ = make_sharpen_constants(settings_, upscale.output);
    if (!upscale.history_valid) {
        ++report_.history_resets;
    }
    if (!(report_.render == upscale.render)) {
        if (report_.frames_declared > 0) {
            ++report_.render_changes;
        }
        report_.render = upscale.render;
    }
    report_.reactive = upscale.reactive;
    return ok();
}

PassId TemporalUpscalePass::declare(RenderGraph& graph,
                                    const ScreenSpaceStageInputs& inputs) noexcept {
    // Each refusal is a frame that would otherwise reconstruct from something nothing wrote, or
    // into a history of another extent.
    if (device_ == nullptr || framework_ == nullptr || inputs.source == kInvalidResource ||
        inputs.depth == kInvalidResource || inputs.target == kInvalidResource ||
        inputs.width != desc_.output.width || inputs.height != desc_.output.height ||
        inputs.render_width == 0 || inputs.render_height == 0 ||
        inputs.render_width > inputs.width || inputs.render_height > inputs.height ||
        ((inputs.output_depth == kInvalidResource) !=
         (inputs.output_velocity == kInvalidResource))) {
        return kInvalidPass;
    }
    if (!prepare(inputs)) {
        return kInvalidPass;
    }
    const u32 write = read_ ^ 1U;
    TextureRequest request;
    request.width = desc_.output.width;
    request.height = desc_.output.height;
    request.format = rhi::Format::Rgba16Sfloat;
    request.extra_usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage;
    request.name = "upscale previous history";
    const ResourceId previous_history =
        graph.import_texture(request, histories_[read_], history_uses_[read_]);
    request.name = "upscale history";
    // UNDEFINED, and it is the truth: the reconstruction writes every texel.
    const ResourceId history =
        graph.import_texture(request, histories_[write], rhi::ImageUse::Undefined);
    request.format = rhi::Format::Rg32Sfloat;
    request.name = "upscale previous state";
    const ResourceId previous_state =
        graph.import_texture(request, states_[read_], state_uses_[read_]);
    request.name = "upscale state";
    const ResourceId state =
        graph.import_texture(request, states_[write], rhi::ImageUse::Undefined);

    Step base;
    base.self = this;
    base.color = inputs.source;
    base.depth = inputs.depth;
    base.velocity = inputs.velocity;
    base.opaque = inputs.opaque_color;
    base.previous_history = previous_history;
    base.previous_state = previous_state;
    base.state = state;
    steps_[kReconstruct] = base;
    steps_[kReconstruct].stage = kReconstruct;
    steps_[kReconstruct].output = history;
    steps_[kReconstruct].width = inputs.width;
    steps_[kReconstruct].height = inputs.height;
    steps_[kSharpen] = base;
    steps_[kSharpen].stage = kSharpen;
    steps_[kSharpen].color = history;
    steps_[kSharpen].output = inputs.target;
    steps_[kSharpen].width = inputs.width;
    steps_[kSharpen].height = inputs.height;

    PassBuilder reconstruct = graph.add_pass("upscale reconstruct", QueueKind::Graphics);
    reconstruct.read(inputs.source, Access::ComputeSampledRead)
        .read(inputs.depth, Access::ComputeSampledRead)
        .read(previous_history, Access::ComputeSampledRead)
        .read(previous_state, Access::ComputeSampledRead);
    if (inputs.velocity != kInvalidResource) {
        reconstruct.read(inputs.velocity, Access::ComputeSampledRead);
    }
    if (inputs.opaque_color != kInvalidResource) {
        reconstruct.read(inputs.opaque_color, Access::ComputeSampledRead);
    }
    const PassId first = reconstruct.write(history, Access::ComputeStorageWrite)
                             .write(state, Access::ComputeStorageWrite)
                             .record(&record_step, &steps_[kReconstruct])
                             .id();
    graph.add_pass("upscale sharpen", QueueKind::Graphics)
        .read(history, Access::ComputeSampledRead)
        .write(inputs.target, Access::ComputeStorageWrite)
        .record(&record_step, &steps_[kSharpen]);
    report_.resampled = inputs.output_depth != kInvalidResource;
    if (report_.resampled) {
        declare_resample(graph, inputs);
    }

    // What this frame leaves each history in, committed by `executed()`.
    pending_history_uses_[read_] = rhi::ImageUse::SampledRead;
    pending_history_uses_[write] = rhi::ImageUse::SampledRead;
    pending_state_uses_[read_] = rhi::ImageUse::SampledRead;
    pending_state_uses_[write] = rhi::ImageUse::Storage;
    if (desc_.readback) {
        declare_readbacks(graph, inputs,
                          HistoryResources{previous_history, history, previous_state, state});
        pending_history_uses_[read_] = rhi::ImageUse::TransferSource;
        pending_history_uses_[write] = rhi::ImageUse::TransferSource;
        pending_state_uses_[read_] = rhi::ImageUse::TransferSource;
        pending_state_uses_[write] = rhi::ImageUse::TransferSource;
        pending_readback_render_ = Extent2D{inputs.render_width, inputs.render_height};
    }
    declared_render_ = Extent2D{inputs.render_width, inputs.render_height};
    declared_ = true;
    ++report_.frames_declared;
    return first;
}

void TemporalUpscalePass::declare_resample(RenderGraph& graph,
                                           const ScreenSpaceStageInputs& inputs) noexcept {
    resample_ =
        Resample{this, inputs.depth, inputs.velocity, inputs.output_depth, inputs.output_velocity};
    PassBuilder builder = graph.add_pass("upscale resample", QueueKind::Graphics);
    builder.read(inputs.depth, Access::FragmentSampledRead);
    if (inputs.velocity != kInvalidResource) {
        builder.read(inputs.velocity, Access::FragmentSampledRead);
    }
    builder.write(inputs.output_depth, Access::DepthStencilAttachmentWrite)
        .write(inputs.output_velocity, Access::ColorAttachmentWrite)
        .record(&record_resample, &resample_);
}

void TemporalUpscalePass::declare_readbacks(RenderGraph& graph,
                                            const ScreenSpaceStageInputs& inputs,
                                            const HistoryResources& kept) noexcept {
    const ResourceId sources[kCopyCount] = {
        inputs.source, inputs.depth,        inputs.velocity,       inputs.opaque_color,
        kept.previous, kept.current,        kept.previous_state,   kept.state,
        inputs.target, inputs.output_depth, inputs.output_velocity};
    const Extent2D render{inputs.render_width, inputs.render_height};
    const Extent2D extents[kCopyCount] = {render,       render,       render,       render,
                                          desc_.output, desc_.output, desc_.output, desc_.output,
                                          desc_.output, desc_.output, desc_.output};
    for (u32 copy = 0; copy < kCopyCount; ++copy) {
        if (sources[copy] == kInvalidResource) {
            copies_[copy] = Readback{};
            continue;
        }
        copies_[copy] = Readback{this, sources[copy], static_cast<Copy>(copy), extents[copy].width,
                                 extents[copy].height};
        BufferRequest request;
        request.name = kCopyNames[copy];
        request.size =
            static_cast<u64>(desc_.output.width) * desc_.output.height * kCopyTexelBytes[copy];
        request.extra_usage = rhi::BufferUsage::TransferDestination;
        const ResourceId destination = graph.import_buffer(request, readbacks_[copy]);
        graph.add_pass(kCopyNames[copy], QueueKind::Graphics)
            .read(sources[copy], Access::TransferRead)
            .write(destination, Access::TransferWrite)
            .record(&record_readback, &copies_[copy]);
        // The host boundary as a declared read, so the graph derives the transfer-to-host barrier.
        graph.add_pass("upscale host", QueueKind::Graphics)
            .read(destination, Access::HostRead)
            .side_effect();
    }
}

void TemporalUpscalePass::executed() noexcept {
    if (!declared_) {
        return;
    }
    declared_ = false;
    for (u32 index = 0; index < 2U; ++index) {
        history_uses_[index] = pending_history_uses_[index];
        state_uses_[index] = pending_state_uses_[index];
    }
    read_ ^= 1U;
    written_ = true;
    readback_render_ = pending_readback_render_;
    if (framework_ != nullptr) {
        framework_->mark_history_written(colour_history_);
        framework_->mark_history_written(state_history_);
    }
    ++report_.frames_executed;
}

void TemporalUpscalePass::record_step(const PassContext& context, void* user) noexcept {
    const auto* step = static_cast<const Step*>(user);
    TemporalUpscalePass& self = *step->self;
    const u32 stage = step->stage;
    Expected<rhi::DescriptorSetHandle, Error> set =
        self.device_->allocate_descriptor_set(self.set_layouts_[stage], true);
    if (!set.has_value()) {
        return;
    }
    const GraphExecutor& executor = *context.executor;
    rhi::DescriptorWrite writes[kMaxBindings];
    u32 count = 0;
    const void* push = &self.constants_;
    u32 copy_extent[4] = {step->width, step->height, 0, 0};
    if (stage == kReconstruct) {
        const rhi::TextureViewHandle colour = executor.view(step->color);
        writes[count++] = sampled(0, colour);
        writes[count++] = sampled(1, executor.view(step->depth));
        // An absent input is bound to the colour, which has a view of the right dimensionality and
        // a layout the graph has already made readable; the flags keep the shader from reading it.
        writes[count++] =
            sampled(2, step->velocity != kInvalidResource ? executor.view(step->velocity) : colour);
        writes[count++] =
            sampled(3, step->opaque != kInvalidResource ? executor.view(step->opaque) : colour);
        writes[count++] = sampled(4, executor.view(step->previous_history));
        writes[count++] = sampled(5, executor.view(step->previous_state));
        writes[count++] = storage(6, executor.view(step->output));
        writes[count++] = storage(7, executor.view(step->state));
    } else if (stage == kSharpen) {
        writes[count++] = sampled(0, executor.view(step->color));
        writes[count++] = storage(1, executor.view(step->output));
        push = &self.sharpen_;
    } else {
        writes[count++] = sampled(0, executor.view(step->color));
        writes[count++] = storage(1, executor.view(step->output));
        push = copy_extent;
    }
    if (Status written = self.device_->update_descriptor_set(
            *set, Span<const rhi::DescriptorWrite>(writes, count));
        !written) {
        return;
    }
    context.commands->bind_compute_pipeline(self.pipelines_[stage]);
    context.commands->bind_descriptor_sets(self.pipeline_layouts_[stage], 0,
                                           Span<const rhi::DescriptorSetHandle>(&*set, 1));
    context.commands->push_constants(
        self.pipeline_layouts_[stage], rhi::ShaderStage::Compute, 0,
        Span<const u8>(static_cast<const u8*>(push), push_bytes(stage)));
    context.commands->dispatch(groups(step->width), groups(step->height), 1);
}

void TemporalUpscalePass::record_resample(const PassContext& context, void* user) noexcept {
    const auto* resample = static_cast<const Resample*>(user);
    TemporalUpscalePass& self = *resample->self;
    Expected<rhi::DescriptorSetHandle, Error> set =
        self.device_->allocate_descriptor_set(self.resample_set_layout_, true);
    if (!set.has_value()) {
        return;
    }
    const GraphExecutor& executor = *context.executor;
    const rhi::TextureViewHandle depth = executor.view(resample->depth);
    const rhi::DescriptorWrite writes[2] = {
        sampled(0, depth),
        // Without motion vectors the depth stands in for the binding, as in the reconstruction.
        sampled(
            1, resample->velocity != kInvalidResource ? executor.view(resample->velocity) : depth)};
    if (Status written =
            self.device_->update_descriptor_set(*set, Span<const rhi::DescriptorWrite>(writes, 2));
        !written) {
        return;
    }
    const u32 width = self.desc_.output.width;
    const u32 height = self.desc_.output.height;
    rhi::RenderAttachment velocity;
    velocity.view = executor.view(resample->output_velocity);
    velocity.load = rhi::LoadOp::Clear;
    velocity.store = rhi::StoreOp::Store;
    rhi::RenderAttachment depth_target;
    depth_target.view = executor.view(resample->output_depth);
    depth_target.load = rhi::LoadOp::Clear;
    depth_target.store = rhi::StoreOp::Store;
    depth_target.clear = rhi::reversed_z_depth_clear();
    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, width, height};
    info.color_attachments = Span<const rhi::RenderAttachment>(&velocity, 1);
    info.depth_attachment = depth_target;
    context.commands->begin_rendering(info);
    context.commands->set_viewport(
        rhi::Viewport{0.0F, 0.0F, static_cast<f32>(width), static_cast<f32>(height), 0.0F, 1.0F});
    context.commands->set_scissor(rhi::Rect2D{0, 0, width, height});
    context.commands->bind_graphics_pipeline(self.resample_pipeline_);
    context.commands->bind_descriptor_sets(self.resample_layout_, 0,
                                           Span<const rhi::DescriptorSetHandle>(&*set, 1));
    context.commands->push_constants(
        self.resample_layout_, rhi::ShaderStage::Fragment, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&self.constants_), sizeof(UpscaleConstants)));
    context.commands->draw(3, 1, 0, 0);
    context.commands->end_rendering();
}

void TemporalUpscalePass::record_readback(const PassContext& context, void* user) noexcept {
    const auto* copy = static_cast<const Readback*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{copy->width, copy->height, 1};
    context.commands->copy_texture_to_buffer(context.executor->texture(copy->source),
                                             copy->self->readbacks_[copy->copy],
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

Status TemporalUpscalePass::read_back(const UpscaleReadback& out) const noexcept {
    if (device_ == nullptr || readbacks_[kCopyOutput].is_null()) {
        return fail(ErrorCode::Unavailable,
                    "temporal upscaler: created without `readback`, so there is nothing to read");
    }
    const usize render = static_cast<usize>(readback_render_.width) * readback_render_.height;
    const usize output = static_cast<usize>(desc_.output.width) * desc_.output.height;
    const usize sizes[kCopyCount] = {out.color.size(),
                                     out.depth.size(),
                                     out.velocity.size(),
                                     out.opaque.size(),
                                     out.previous_history.size(),
                                     out.history.size(),
                                     out.previous_state.size(),
                                     out.state.size(),
                                     out.output.size(),
                                     out.output_depth.size(),
                                     out.output_velocity.size()};
    const void* mapped[kCopyCount] = {};
    for (u32 copy = 0; copy < kCopyCount; ++copy) {
        const usize expected = copy <= kCopyOpaque ? render : output;
        if (sizes[copy] != 0 && sizes[copy] != expected) {
            return fail(ErrorCode::InvalidArgument,
                        "temporal upscaler: a readback span is neither empty nor its extent");
        }
        if (sizes[copy] == 0) {
            continue;
        }
        mapped[copy] = device_->buffer_mapped_pointer(readbacks_[copy]);
        if (mapped[copy] == nullptr) {
            return fail(ErrorCode::Internal, "temporal upscaler: a readback buffer is not mapped");
        }
    }
    read_colour(mapped[kCopyColor], out.color);
    read_colour(mapped[kCopyOpaque], out.opaque);
    read_colour(mapped[kCopyPreviousHistory], out.previous_history);
    read_colour(mapped[kCopyHistory], out.history);
    read_colour(mapped[kCopyOutput], out.output);
    read_motion(mapped[kCopyVelocity], out.velocity);
    read_motion(mapped[kCopyOutputVelocity], out.output_velocity);
    read_floats(mapped[kCopyDepth], out.depth);
    read_floats(mapped[kCopyPreviousState], out.previous_state);
    read_floats(mapped[kCopyState], out.state);
    read_floats(mapped[kCopyOutputDepth], out.output_depth);
    return ok();
}

}  // namespace cy::rendering::upscale
