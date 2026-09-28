// SPDX-License-Identifier: MIT
#include <cy/rendering/motion_blur/motion_blur_pass.h>

#include <cy/backends/rhi/validation.h>
#include <cy/rendering/graph/executor.h>

#include "motion_blur_msl.h"
#include "motion_blur_spirv.h"

#include <cstring>

namespace cy::rendering::motion_blur {
namespace {

using rhi::Access;
using rhi::QueueKind;

constexpr u32 kGroupSize = 8;
/// The most bindings a stage's set has — the gather's colour, velocity, depth, neighbourhood and
/// output. Each set is its shader's `ParameterBlock` in field order, which is also the argument
/// buffer id Metal assigns, and the last binding of each is the storage image it writes.
constexpr u32 kMaxBindings = 5;

struct StageShader {
    const char* name;
    const char* entry;
    Span<const u32> spirv;
    const char* msl;
    usize msl_bytes;
    u32 bindings;
};

[[nodiscard]] StageShader stage_shader(u32 stage) noexcept {
    switch (stage) {
        case 0:
            return {"motion blur tile max",
                    "cyMotionBlurTileMax",
                    {kMotionBlurTileMaxSpirv, sizeof(kMotionBlurTileMaxSpirv) / sizeof(u32)},
                    kMotionBlurTileMaxMsl,
                    sizeof(kMotionBlurTileMaxMsl) - 1,
                    3};
        case 1:
            return {
                "motion blur neighbour max",
                "cyMotionBlurNeighbourMax",
                {kMotionBlurNeighbourMaxSpirv, sizeof(kMotionBlurNeighbourMaxSpirv) / sizeof(u32)},
                kMotionBlurNeighbourMaxMsl,
                sizeof(kMotionBlurNeighbourMaxMsl) - 1,
                2};
        case 2:
            return {"motion blur gather",
                    "cyMotionBlurGather",
                    {kMotionBlurGatherSpirv, sizeof(kMotionBlurGatherSpirv) / sizeof(u32)},
                    kMotionBlurGatherMsl,
                    sizeof(kMotionBlurGatherMsl) - 1,
                    5};
        default:
            return {"motion blur readback copy",
                    "cyMotionBlurCopy",
                    {kMotionBlurCopySpirv, sizeof(kMotionBlurCopySpirv) / sizeof(u32)},
                    kMotionBlurCopyMsl,
                    sizeof(kMotionBlurCopyMsl) - 1,
                    2};
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

/// Bytes a texel of each read-back image is: two RGBA16F colours, the RG16F velocity, D32 depth.
constexpr u64 kCopyTexelBytes[] = {8U, 4U, 4U, 8U};
constexpr const char* kCopyNames[] = {"motion blur input readback", "motion blur velocity readback",
                                      "motion blur depth readback", "motion blur output readback"};

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
            // Subnormal: renormalise into binary32's range.
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

void read_colour(const u16* texels, Span<Vec4> out) noexcept {
    for (usize pixel = 0; pixel < out.size(); ++pixel) {
        const u16* texel = texels + (pixel * 4U);
        out[pixel] = Vec4{half_to_float(texel[0]), half_to_float(texel[1]), half_to_float(texel[2]),
                          half_to_float(texel[3])};
    }
}

}  // namespace

MotionBlurPass::~MotionBlurPass() {
    destroy();
}

bool MotionBlurPass::supported(const rhi::Device& device) noexcept {
    const rhi::ShaderFormat format = device.capabilities().native_shader_format();
    return device.capabilities().has(rhi::Capability::ComputeShaders) &&
           (format == rhi::ShaderFormat::Spirv || format == rhi::ShaderFormat::Msl);
}

Status MotionBlurPass::create(Allocator& allocator, rhi::Device& device,
                              const MotionBlurPassDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the motion blur pass is already created");
    }
    if (!supported(device)) {
        return fail(ErrorCode::Unsupported,
                    "motion blur is three compute dispatches and this device has no compute stage "
                    "or no shader format this module ships (SPIR-V or MSL)");
    }
    if (desc.width == 0 || desc.height == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "MotionBlurPassDescription::width and ::height must be non-zero");
    }
    allocator_ = &allocator;
    device_ = &device;
    desc_ = desc;
    // The readback copy exists only for a pass that reads back.
    const u32 stages = desc.readback ? u32{kStageCount} : u32{kCopy};
    for (u32 stage = 0; stage < stages; ++stage) {
        if (Status created = create_pipeline(static_cast<Stage>(stage)); !created) {
            destroy();
            return created;
        }
    }
    if (Status created = create_resources(); !created) {
        destroy();
        return created;
    }
    return ok();
}

Status MotionBlurPass::create_pipeline(Stage stage) noexcept {
    const StageShader shader = stage_shader(stage);
    rhi::ShaderModuleBundle bundle;
    bundle.spirv = shader.spirv;
    bundle.msl = {reinterpret_cast<const u8*>(shader.msl), shader.msl_bytes};
    bundle.msl_entry_point = shader.entry;
    bundle.spirv_entry_point = "main";
    rhi::ValidationMessage message;
    auto module = rhi::select_shader_module(bundle, device_->capabilities().native_shader_format(),
                                            shader.name, rhi::ShaderStage::Compute, message);
    if (!module.has_value()) {
        return make_unexpected(module.error());
    }
    Expected<rhi::ShaderModuleHandle, Error> made_shader = device_->create_shader_module(*module);
    if (!made_shader.has_value()) {
        return make_unexpected(made_shader.error());
    }
    shaders_[stage] = *made_shader;

    rhi::DescriptorBinding bindings[kMaxBindings] = {};
    for (u32 index = 0; index < shader.bindings; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = index + 1U == shader.bindings ? rhi::DescriptorKind::StorageTexture
                                                             : rhi::DescriptorKind::SampledTexture;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Compute;
    }
    rhi::DescriptorSetLayoutDescription set_layout;
    set_layout.name = shader.name;
    set_layout.bindings = Span<const rhi::DescriptorBinding>(bindings, shader.bindings);
    Expected<rhi::DescriptorSetLayoutHandle, Error> layout =
        device_->create_descriptor_set_layout(set_layout);
    if (!layout.has_value()) {
        return make_unexpected(layout.error());
    }
    set_layouts_[stage] = *layout;

    const rhi::PushConstantRange range{rhi::ShaderStage::Compute, 0,
                                       static_cast<u32>(sizeof(MotionBlurConstants))};
    rhi::PipelineLayoutDescription pipeline_layout;
    pipeline_layout.name = shader.name;
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
    pipeline.name = shader.name;
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

Status MotionBlurPass::create_resources() noexcept {
    if (!desc_.readback) {
        return ok();
    }
    const u64 pixels = static_cast<u64>(desc_.width) * desc_.height;
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

void MotionBlurPass::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    for (rhi::BufferHandle& buffer : readbacks_) {
        if (!buffer.is_null()) {
            device_->destroy_buffer(buffer);
        }
        buffer = {};
    }
    for (u32 stage = 0; stage < kStageCount; ++stage) {
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
    target_ = kInvalidResource;
    device_ = nullptr;
    allocator_ = nullptr;
}

Status MotionBlurPass::set_view(const MotionBlurView& view) noexcept {
    if (view.width != desc_.width || view.height != desc_.height) {
        return fail(ErrorCode::InvalidArgument,
                    "motion blur: the view's extent is not the one the pass was created at");
    }
    Expected<MotionBlurConstants, Error> constants = make_motion_blur_constants(settings_, view);
    if (!constants.has_value()) {
        return make_unexpected(constants.error());
    }
    view_ = view;
    constants_ = *constants;
    return ok();
}

ResourceId MotionBlurPass::declare_target(RenderGraph& graph) noexcept {
    TextureRequest request;
    request.name = "motion blur";
    request.format = target_format();
    request.width = desc_.width;
    request.height = desc_.height;
    // The gather writes every texel, and the usage is what the frame's passes declare of it.
    target_ = graph.create_texture(request);
    return target_;
}

FrameStageDeclaration MotionBlurPass::stage() noexcept {
    return FrameStageDeclaration{&declare_stage, this};
}

PassId MotionBlurPass::declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                     void* user) noexcept {
    return static_cast<MotionBlurPass*>(user)->declare(graph, inputs);
}

PassId MotionBlurPass::declare(RenderGraph& graph, const ScreenSpaceStageInputs& inputs) noexcept {
    // Each refusal is a frame that would otherwise blur with motion nothing wrote, or post-process
    // a target nothing wrote.
    if (device_ == nullptr || view_.width == 0 || inputs.target == kInvalidResource ||
        inputs.target != target_ || inputs.velocity == kInvalidResource ||
        inputs.color == kInvalidResource || inputs.depth == kInvalidResource ||
        inputs.width != desc_.width || inputs.height != desc_.height) {
        return kInvalidPass;
    }
    TextureRequest request;
    request.format = rhi::Format::Rgba32Sfloat;
    request.width = constants_.control[2];
    request.height = constants_.control[3];
    request.name = "motion blur tile max";
    const ResourceId tiles = graph.create_texture(request);
    request.name = "motion blur neighbour max";
    const ResourceId neighbours = graph.create_texture(request);

    const Step base{this,         kTileMax, inputs.color, inputs.velocity,
                    inputs.depth, tiles,    neighbours,   inputs.target};
    for (u32 stage = 0; stage < kStageCount; ++stage) {
        steps_[stage] = base;
        steps_[stage].stage = static_cast<Stage>(stage);
    }
    if (desc_.readback) {
        request.format = target_format();
        request.width = desc_.width;
        request.height = desc_.height;
        request.name = "motion blur input copy";
        steps_[kCopy].copy = graph.create_texture(request);
    }
    const PassId first = graph.add_pass("motion blur tile max", QueueKind::Graphics)
                             .read(inputs.velocity, Access::ComputeSampledRead)
                             .read(inputs.depth, Access::ComputeSampledRead)
                             .write(tiles, Access::ComputeStorageWrite)
                             .record(&record_step, &steps_[kTileMax])
                             .id();
    graph.add_pass("motion blur neighbour max", QueueKind::Graphics)
        .read(tiles, Access::ComputeSampledRead)
        .write(neighbours, Access::ComputeStorageWrite)
        .record(&record_step, &steps_[kNeighbourMax]);
    graph.add_pass("motion blur gather", QueueKind::Graphics)
        .read(inputs.color, Access::ComputeSampledRead)
        .read(inputs.velocity, Access::ComputeSampledRead)
        .read(inputs.depth, Access::ComputeSampledRead)
        .read(neighbours, Access::ComputeSampledRead)
        .write(inputs.target, Access::ComputeStorageWrite)
        .record(&record_step, &steps_[kGather]);
    if (desc_.readback) {
        graph.add_pass("motion blur readback copy", QueueKind::Graphics)
            .read(inputs.color, Access::ComputeSampledRead)
            .write(steps_[kCopy].copy, Access::ComputeStorageWrite)
            .record(&record_step, &steps_[kCopy]);
        declare_readbacks(graph, inputs);
    }
    return first;
}

void MotionBlurPass::declare_readbacks(RenderGraph& graph,
                                       const ScreenSpaceStageInputs& inputs) noexcept {
    const ResourceId sources[kCopyCount] = {steps_[kCopy].copy, inputs.velocity, inputs.depth,
                                            inputs.target};
    const u64 pixels = static_cast<u64>(desc_.width) * desc_.height;
    for (u32 copy = 0; copy < kCopyCount; ++copy) {
        copies_[copy] = Readback{this, sources[copy], static_cast<Copy>(copy)};
        BufferRequest request;
        request.name = kCopyNames[copy];
        request.size = pixels * kCopyTexelBytes[copy];
        request.extra_usage = rhi::BufferUsage::TransferDestination;
        const ResourceId destination = graph.import_buffer(request, readbacks_[copy]);
        graph.add_pass(kCopyNames[copy], QueueKind::Graphics)
            .read(sources[copy], Access::TransferRead)
            .write(destination, Access::TransferWrite)
            .record(&record_readback, &copies_[copy]);
        // The host boundary as a declared read, so the graph derives the transfer-to-host barrier.
        graph.add_pass("motion blur host", QueueKind::Graphics)
            .read(destination, Access::HostRead)
            .side_effect();
    }
}

void MotionBlurPass::record_step(const PassContext& context, void* user) noexcept {
    const auto* step = static_cast<const Step*>(user);
    MotionBlurPass& self = *step->self;
    const u32 stage = step->stage;
    Expected<rhi::DescriptorSetHandle, Error> set =
        self.device_->allocate_descriptor_set(self.set_layouts_[stage], true);
    if (!set.has_value()) {
        return;
    }
    const GraphExecutor& executor = *context.executor;
    rhi::DescriptorWrite writes[kMaxBindings];
    u32 count = 0;
    u32 across = groups(self.constants_.control[2]);
    u32 down = groups(self.constants_.control[3]);
    if (stage == kTileMax) {
        writes[count++] = sampled(0, executor.view(step->velocity));
        writes[count++] = sampled(1, executor.view(step->depth));
        writes[count++] = storage(2, executor.view(step->tiles));
    } else if (stage == kNeighbourMax) {
        writes[count++] = sampled(0, executor.view(step->tiles));
        writes[count++] = storage(1, executor.view(step->neighbours));
    } else if (stage == kCopy) {
        writes[count++] = sampled(0, executor.view(step->color));
        writes[count++] = storage(1, executor.view(step->copy));
        across = groups(self.desc_.width);
        down = groups(self.desc_.height);
    } else {
        writes[count++] = sampled(0, executor.view(step->color));
        writes[count++] = sampled(1, executor.view(step->velocity));
        writes[count++] = sampled(2, executor.view(step->depth));
        writes[count++] = sampled(3, executor.view(step->neighbours));
        writes[count++] = storage(4, executor.view(step->output));
        across = groups(self.desc_.width);
        down = groups(self.desc_.height);
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
        Span<const u8>(reinterpret_cast<const u8*>(&self.constants_), sizeof(MotionBlurConstants)));
    context.commands->dispatch(across, down, 1);
}

void MotionBlurPass::record_readback(const PassContext& context, void* user) noexcept {
    const auto* copy = static_cast<const Readback*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{copy->self->desc_.width, copy->self->desc_.height, 1};
    context.commands->copy_texture_to_buffer(context.executor->texture(copy->source),
                                             copy->self->readbacks_[copy->copy],
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

Status MotionBlurPass::read_back(const MotionBlurReadback& out) const noexcept {
    if (device_ == nullptr || readbacks_[kCopyOutput].is_null()) {
        return fail(ErrorCode::Unavailable,
                    "motion blur: created without `readback`, so there is nothing to read");
    }
    const usize pixels = static_cast<usize>(desc_.width) * desc_.height;
    const usize sizes[kCopyCount] = {out.input.size(), out.velocity.size(), out.depth.size(),
                                     out.output.size()};
    const void* mapped[kCopyCount] = {};
    for (u32 copy = 0; copy < kCopyCount; ++copy) {
        if (sizes[copy] != 0 && sizes[copy] != pixels) {
            return fail(ErrorCode::InvalidArgument,
                        "motion blur: a readback span is neither empty nor the view");
        }
        mapped[copy] = device_->buffer_mapped_pointer(readbacks_[copy]);
        if (mapped[copy] == nullptr) {
            return fail(ErrorCode::Internal, "motion blur: a readback buffer is not mapped");
        }
    }
    read_colour(static_cast<const u16*>(mapped[kCopyInput]), out.input);
    read_colour(static_cast<const u16*>(mapped[kCopyOutput]), out.output);
    const auto* velocity = static_cast<const u16*>(mapped[kCopyVelocity]);
    for (usize pixel = 0; pixel < out.velocity.size(); ++pixel) {
        out.velocity[pixel] =
            Vec2{half_to_float(velocity[pixel * 2U]), half_to_float(velocity[(pixel * 2U) + 1U])};
    }
    if (!out.depth.empty()) {
        std::memcpy(out.depth.data(), mapped[kCopyDepth], pixels * sizeof(f32));
    }
    return ok();
}

}  // namespace cy::rendering::motion_blur
