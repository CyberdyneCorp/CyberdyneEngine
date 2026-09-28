// SPDX-License-Identifier: MIT
#include <cy/rendering/depth_of_field/dof_pass.h>

#include <cy/backends/rhi/validation.h>
#include <cy/rendering/graph/executor.h>

#include "dof_msl.h"
#include "dof_spirv.h"

namespace cy::rendering::depth_of_field {
namespace {

using rhi::Access;
using rhi::QueueKind;

constexpr u32 kGroupSize = 8;
constexpr u32 kMaxBindings = 6;
/// Every intermediate image: linear HDR colour and a signed radius, or two radii.
constexpr rhi::Format kLayerFormat = rhi::Format::Rgba16Sfloat;

/// One dispatch's shader and set: the `ParameterBlock`'s fields in order — sampled images first,
/// storage images last — which is also the argument-buffer id Metal assigns.
struct StepShape {
    const char* name;
    const char* entry;
    Span<const u32> spirv;
    Span<const u8> msl;
    u32 sampled;
    u32 storage;
};

template <usize N>
[[nodiscard]] Span<const u32> words(const u32 (&module)[N]) noexcept {
    return Span<const u32>(module, N);
}

template <usize N>
[[nodiscard]] Span<const u8> text(const char (&module)[N]) noexcept {
    return {reinterpret_cast<const u8*>(module), N - 1U};
}

[[nodiscard]] StepShape shape(const char* name, const char* entry, Span<const u32> spirv,
                              Span<const u8> msl, u32 sampled, u32 storage) noexcept {
    return StepShape{name, entry, spirv, msl, sampled, storage};
}

[[nodiscard]] StepShape shape_of(DofStep step) noexcept {
    switch (step) {
        case DofStep::Setup:
            return shape("depth of field setup", "cyDofSetup", words(kDofSetupSpirv),
                         text(kDofSetupMsl), 2, 1);
        case DofStep::Tiles:
            return shape("depth of field tiles", "cyDofTiles", words(kDofTilesSpirv),
                         text(kDofTilesMsl), 1, 1);
        case DofStep::Dilate:
            return shape("depth of field dilate", "cyDofDilate", words(kDofDilateSpirv),
                         text(kDofDilateMsl), 1, 1);
        case DofStep::Gather:
            return shape("depth of field gather", "cyDofGather", words(kDofGatherSpirv),
                         text(kDofGatherMsl), 2, 2);
        case DofStep::Composite:
        case DofStep::Count:
            break;
    }
    return shape("depth of field composite", "cyDofComposite", words(kDofCompositeSpirv),
                 text(kDofCompositeMsl), 5, 1);
}

[[nodiscard]] u32 groups(u32 extent) noexcept {
    return (extent + kGroupSize - 1U) / kGroupSize;
}

[[nodiscard]] rhi::DescriptorWrite image_write(u32 binding, rhi::TextureViewHandle view,
                                               bool storage) noexcept {
    rhi::DescriptorWrite write;
    write.binding = binding;
    write.kind =
        storage ? rhi::DescriptorKind::StorageTexture : rhi::DescriptorKind::SampledTexture;
    write.texture_view = view;
    write.use = storage ? rhi::ImageUse::Storage : rhi::ImageUse::SampledRead;
    return write;
}

void record_step(const PassContext& context, void* user) noexcept {
    const auto* step = static_cast<const DepthOfFieldPass::Step*>(user);
    step->self->record(step->step, context);
}

}  // namespace

DepthOfFieldPass::~DepthOfFieldPass() {
    destroy();
}

bool DepthOfFieldPass::supported(const rhi::Device& device) noexcept {
    const rhi::ShaderFormat format = device.capabilities().native_shader_format();
    return device.capabilities().has(rhi::Capability::ComputeShaders) &&
           (format == rhi::ShaderFormat::Spirv || format == rhi::ShaderFormat::Msl);
}

Status DepthOfFieldPass::create(rhi::Device& device,
                                const DepthOfFieldPassDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the depth of field pass is already created");
    }
    if (!supported(device)) {
        return fail(ErrorCode::Unsupported,
                    "depth of field is a chain of compute dispatches and this device has no "
                    "compute stage or no shader format this module ships (SPIR-V or MSL)");
    }
    if (desc.width == 0 || desc.height == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "DepthOfFieldPassDescription::width and ::height must be non-zero");
    }
    device_ = &device;
    desc_ = desc;
    for (u32 index = 0; index < kDofStepCount; ++index) {
        steps_[index] = Step{this, static_cast<DofStep>(index)};
        if (Status created = create_pipeline(static_cast<DofStep>(index)); !created) {
            destroy();
            return created;
        }
    }
    return ok();
}

Status DepthOfFieldPass::create_pipeline(DofStep step) noexcept {
    const auto which = static_cast<u32>(step);
    const StepShape shape = shape_of(step);
    rhi::ShaderModuleBundle bundle;
    bundle.spirv = shape.spirv;
    bundle.msl = shape.msl;
    bundle.msl_entry_point = shape.entry;
    bundle.spirv_entry_point = "main";
    rhi::ValidationMessage message;
    auto module = rhi::select_shader_module(bundle, device_->capabilities().native_shader_format(),
                                            shape.name, rhi::ShaderStage::Compute, message);
    if (!module.has_value()) {
        return make_unexpected(module.error());
    }
    Expected<rhi::ShaderModuleHandle, Error> shader = device_->create_shader_module(*module);
    if (!shader.has_value()) {
        return make_unexpected(shader.error());
    }
    shaders_[which] = *shader;

    const u32 count = shape.sampled + shape.storage;
    rhi::DescriptorBinding bindings[kMaxBindings] = {};
    for (u32 index = 0; index < count; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = index < shape.sampled ? rhi::DescriptorKind::SampledTexture
                                                     : rhi::DescriptorKind::StorageTexture;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Compute;
    }
    rhi::DescriptorSetLayoutDescription set_layout;
    set_layout.name = shape.name;
    set_layout.bindings = Span<const rhi::DescriptorBinding>(bindings, count);
    Expected<rhi::DescriptorSetLayoutHandle, Error> layout =
        device_->create_descriptor_set_layout(set_layout);
    if (!layout.has_value()) {
        return make_unexpected(layout.error());
    }
    set_layouts_[which] = *layout;

    const rhi::PushConstantRange range{rhi::ShaderStage::Compute, 0,
                                       static_cast<u32>(sizeof(DofConstants))};
    rhi::PipelineLayoutDescription pipeline_layout;
    pipeline_layout.name = shape.name;
    pipeline_layout.set_layouts =
        Span<const rhi::DescriptorSetLayoutHandle>(&set_layouts_[which], 1);
    pipeline_layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> made =
        device_->create_pipeline_layout(pipeline_layout);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    pipeline_layouts_[which] = *made;

    rhi::ComputePipelineDescription pipeline;
    pipeline.name = shape.name;
    pipeline.layout = pipeline_layouts_[which];
    pipeline.shader = shaders_[which];
    pipeline.workgroup_size[0] = kGroupSize;
    pipeline.workgroup_size[1] = kGroupSize;
    Expected<rhi::ComputePipelineHandle, Error> created =
        device_->create_compute_pipeline(pipeline);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    pipelines_[which] = *created;
    return ok();
}

void DepthOfFieldPass::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    for (u32 which = 0; which < kDofStepCount; ++which) {
        if (!pipelines_[which].is_null()) {
            device_->destroy_compute_pipeline(pipelines_[which]);
        }
        if (!pipeline_layouts_[which].is_null()) {
            device_->destroy_pipeline_layout(pipeline_layouts_[which]);
        }
        if (!set_layouts_[which].is_null()) {
            device_->destroy_descriptor_set_layout(set_layouts_[which]);
        }
        if (!shaders_[which].is_null()) {
            device_->destroy_shader_module(shaders_[which]);
        }
        pipelines_[which] = {};
        pipeline_layouts_[which] = {};
        set_layouts_[which] = {};
        shaders_[which] = {};
    }
    viewed_ = false;
    device_ = nullptr;
}

Status DepthOfFieldPass::set_view(const DofView& view) noexcept {
    if (view.width != desc_.width || view.height != desc_.height) {
        return fail(ErrorCode::InvalidArgument,
                    "depth of field: the view's extent is not the one the pass was created at");
    }
    Expected<DofConstants, Error> constants = make_dof_constants(settings_, view);
    if (!constants.has_value()) {
        return make_unexpected(constants.error());
    }
    constants_ = *constants;
    viewed_ = true;
    return ok();
}

FrameStageDeclaration DepthOfFieldPass::stage() noexcept {
    return FrameStageDeclaration{&declare_stage, this};
}

PassId DepthOfFieldPass::declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                       void* user) noexcept {
    return static_cast<DepthOfFieldPass*>(user)->declare(graph, inputs);
}

PassId DepthOfFieldPass::declare(RenderGraph& graph,
                                 const ScreenSpaceStageInputs& inputs) noexcept {
    report_.passes = 0;
    // Each refusal is a frame that would otherwise blur by a lens it was not given, or read an
    // image nothing wrote.
    if (device_ == nullptr || !viewed_ || inputs.source == kInvalidResource ||
        inputs.target == kInvalidResource || inputs.depth == kInvalidResource ||
        inputs.width != desc_.width || inputs.height != desc_.height) {
        return kInvalidPass;
    }
    // The composite writes the target as `rgba16f` storage, and the setup reads the source as
    // linear HDR: a frame whose scene colour is another format is refused rather than converted.
    if (graph.resource(inputs.target).texture.format != kLayerFormat) {
        return kInvalidPass;
    }
    source_ = inputs.source;
    depth_ = inputs.depth;
    target_ = inputs.target;

    TextureRequest request;
    request.format = kLayerFormat;
    request.extra_usage = rhi::TextureUsage::Storage;
    request.width = constants_.extent[2];
    request.height = constants_.extent[3];
    request.name = "depth of field layer";
    layer_ = graph.create_texture(request);
    request.name = "depth of field far";
    far_ = graph.create_texture(request);
    request.name = "depth of field near";
    near_ = graph.create_texture(request);
    request.width = constants_.tiles[1];
    request.height = constants_.tiles[2];
    request.name = "depth of field tiles";
    tiles_ = graph.create_texture(request);
    request.name = "depth of field dilated tiles";
    dilated_ = graph.create_texture(request);

    const PassId first = graph.add_pass("depth of field setup", QueueKind::Graphics)
                             .read(source_, Access::ComputeSampledRead)
                             .read(depth_, Access::ComputeSampledRead)
                             .write(layer_, Access::ComputeStorageWrite)
                             .record(&record_step, &steps_[0])
                             .id();
    graph.add_pass("depth of field tiles", QueueKind::Graphics)
        .read(layer_, Access::ComputeSampledRead)
        .write(tiles_, Access::ComputeStorageWrite)
        .record(&record_step, &steps_[1]);
    graph.add_pass("depth of field dilate", QueueKind::Graphics)
        .read(tiles_, Access::ComputeSampledRead)
        .write(dilated_, Access::ComputeStorageWrite)
        .record(&record_step, &steps_[2]);
    graph.add_pass("depth of field gather", QueueKind::Graphics)
        .read(layer_, Access::ComputeSampledRead)
        .read(dilated_, Access::ComputeSampledRead)
        .write(far_, Access::ComputeStorageWrite)
        .write(near_, Access::ComputeStorageWrite)
        .record(&record_step, &steps_[3]);
    graph.add_pass("depth of field composite", QueueKind::Graphics)
        .read(source_, Access::ComputeSampledRead)
        .read(depth_, Access::ComputeSampledRead)
        .read(layer_, Access::ComputeSampledRead)
        .read(far_, Access::ComputeSampledRead)
        .read(near_, Access::ComputeSampledRead)
        .write(target_, Access::ComputeStorageWrite)
        .record(&record_step, &steps_[4]);
    report_.passes = kDofStepCount;
    return first;
}

void DepthOfFieldPass::record(DofStep step, const PassContext& context) noexcept {
    const auto which = static_cast<u32>(step);
    const GraphExecutor& executor = *context.executor;
    ResourceId images[kMaxBindings] = {};
    u32 count = 0;
    u32 groups_x = 0;
    u32 groups_y = 0;
    switch (step) {
        case DofStep::Setup:
            images[count++] = source_;
            images[count++] = depth_;
            images[count++] = layer_;
            groups_x = groups(constants_.extent[2]);
            groups_y = groups(constants_.extent[3]);
            break;
        case DofStep::Tiles:
            images[count++] = layer_;
            images[count++] = tiles_;
            groups_x = groups(constants_.tiles[1]);
            groups_y = groups(constants_.tiles[2]);
            break;
        case DofStep::Dilate:
            images[count++] = tiles_;
            images[count++] = dilated_;
            groups_x = groups(constants_.tiles[1]);
            groups_y = groups(constants_.tiles[2]);
            break;
        case DofStep::Gather:
            images[count++] = layer_;
            images[count++] = dilated_;
            images[count++] = far_;
            images[count++] = near_;
            groups_x = groups(constants_.extent[2]);
            groups_y = groups(constants_.extent[3]);
            break;
        case DofStep::Composite:
            images[count++] = source_;
            images[count++] = depth_;
            images[count++] = layer_;
            images[count++] = far_;
            images[count++] = near_;
            images[count++] = target_;
            groups_x = groups(constants_.extent[0]);
            groups_y = groups(constants_.extent[1]);
            break;
        case DofStep::Count:
            return;
    }
    const u32 sampled = shape_of(step).sampled;
    Expected<rhi::DescriptorSetHandle, Error> set =
        device_->allocate_descriptor_set(set_layouts_[which], true);
    if (!set.has_value()) {
        return;
    }
    rhi::DescriptorWrite writes[kMaxBindings] = {};
    for (u32 index = 0; index < count; ++index) {
        writes[index] = image_write(index, executor.view(images[index]), index >= sampled);
    }
    if (Status written =
            device_->update_descriptor_set(*set, Span<const rhi::DescriptorWrite>(writes, count));
        !written) {
        return;
    }
    rhi::CommandBuffer& commands = *context.commands;
    commands.bind_compute_pipeline(pipelines_[which]);
    commands.bind_descriptor_sets(pipeline_layouts_[which], 0,
                                  Span<const rhi::DescriptorSetHandle>(&*set, 1));
    commands.push_constants(
        pipeline_layouts_[which], rhi::ShaderStage::Compute, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&constants_), sizeof(DofConstants)));
    commands.dispatch(groups_x, groups_y, 1);
    ++report_.dispatches;
}

}  // namespace cy::rendering::depth_of_field
