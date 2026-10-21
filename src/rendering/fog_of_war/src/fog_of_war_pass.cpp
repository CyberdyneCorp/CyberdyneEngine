// SPDX-License-Identifier: MIT
#include <cy/rendering/fog_of_war/fog_of_war_pass.h>

#include <cy/backends/rhi/validation.h>
#include <cy/rendering/graph/executor.h>

#include <cmath>
#include <cstring>

#include "fog_of_war_msl.h"
#include "fog_of_war_spirv.h"

namespace cy::rendering::fog_of_war {
namespace {

using rhi::Access;
using rhi::QueueKind;

constexpr u32 kGroupSize = 8;
/// fog_of_war.slang's set, in the `ParameterBlock`'s field order — which is also the
/// argument-buffer id Metal assigns: the source, the depth, the visibility texture, the words and
/// the target.
constexpr u32 kBindings = 5;
constexpr u64 kWordsBytes = sizeof(FogOfWarConstants);
constexpr rhi::Format kVisibilityFormat = rhi::Format::Rgba8Unorm;
/// The target is written as `rgba16f` storage: a scene colour of another format is refused.
constexpr rhi::Format kTargetFormat = rhi::Format::Rgba16Sfloat;

[[nodiscard]] u32 groups(u32 extent) noexcept {
    return (extent + kGroupSize - 1U) / kGroupSize;
}

[[nodiscard]] bool unit_range(f32 value) noexcept {
    return value >= 0.0F && value <= 1.0F;
}

[[nodiscard]] bool finite(Vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

void record_upload_step(const PassContext& context, void* user) noexcept {
    static_cast<FogOfWarPass*>(user)->record_upload(context);
}

void record_dispatch_step(const PassContext& context, void* user) noexcept {
    static_cast<FogOfWarPass*>(user)->record_dispatch(context);
}

void record_readback_step(const PassContext& context, void* user) noexcept {
    static_cast<FogOfWarPass*>(user)->record_readback(context);
}

}  // namespace

Expected<FogOfWarConstants, Error> pack_fog_of_war_constants(
    const FogOfWarView& view, const FogGridPlacement& placement, const FogOfWarStyle& style,
    u32 target_width, u32 target_height, u32 grid_width, u32 grid_height) noexcept {
    if (target_width == 0 || target_height == 0 || grid_width == 0 || grid_height == 0) {
        return fail(ErrorCode::InvalidArgument, "fog of war: an extent is zero");
    }
    if (!(placement.cell_size > 0.0F) || !std::isfinite(placement.cell_size) ||
        !std::isfinite(placement.origin_x) || !std::isfinite(placement.origin_z)) {
        return fail(ErrorCode::InvalidArgument,
                    "fog of war: the grid's cell size is not positive or its origin not finite");
    }
    if (!unit_range(style.explored_brightness) || !unit_range(style.explored_saturation) ||
        !finite(style.unexplored_colour) || !finite(view.eye)) {
        return fail(ErrorCode::InvalidArgument,
                    "fog of war: the style's brightness and saturation are 0 to 1");
    }
    Expected<Mat4, Error> inverse_clip = inverse(view.projection * view.relative_view);
    if (!inverse_clip.has_value()) {
        return fail(ErrorCode::InvalidArgument, "fog of war: the camera is singular");
    }
    FogOfWarConstants constants;
    for (u32 column = 0; column < 4U; ++column) {
        constants.words[column] = inverse_clip->columns[column];
    }
    constants.words[4] = Vec4{view.eye.x, view.eye.y, view.eye.z, 0.0F};
    constants.words[5] =
        Vec4{placement.origin_x, placement.origin_z, 1.0F / placement.cell_size, 0.0F};
    constants.words[6] = Vec4{style.explored_brightness, style.explored_saturation, 0.0F, 0.0F};
    constants.words[7] =
        Vec4{style.unexplored_colour.x, style.unexplored_colour.y, style.unexplored_colour.z, 0.0F};
    constants.words[8] = Vec4{static_cast<f32>(target_width), static_cast<f32>(target_height),
                              static_cast<f32>(grid_width), static_cast<f32>(grid_height)};
    return constants;
}

FogOfWarPass::~FogOfWarPass() {
    destroy();
}

bool FogOfWarPass::supported(const rhi::Device& device) noexcept {
    const rhi::ShaderFormat format = device.capabilities().native_shader_format();
    return device.capabilities().has(rhi::Capability::ComputeShaders) &&
           (format == rhi::ShaderFormat::Spirv || format == rhi::ShaderFormat::Msl);
}

Status FogOfWarPass::create(Allocator& allocator, rhi::Device& device,
                            const FogOfWarPassDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the fog of war pass is already created");
    }
    if (!supported(device)) {
        return fail(ErrorCode::Unsupported,
                    "fog of war is a compute dispatch and this device has no compute stage or no "
                    "shader format this module ships (SPIR-V or MSL)");
    }
    if (desc.width == 0 || desc.height == 0 || desc.grid_width == 0 || desc.grid_height == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "FogOfWarPassDescription's extent and grid must be non-zero");
    }
    device_ = &device;
    desc_ = desc;
    texels_ = Array<u8>(allocator);
    if (Status sized = texels_.resize(static_cast<usize>(texel_bytes())); !sized) {
        destroy();
        return sized;
    }
    if (Status created = create_pipeline(); !created) {
        destroy();
        return created;
    }
    if (Status created = create_resources(); !created) {
        destroy();
        return created;
    }
    return ok();
}

u64 FogOfWarPass::texel_bytes() const noexcept {
    return static_cast<u64>(desc_.grid_width) * desc_.grid_height * kTexelBytes;
}

u64 FogOfWarPass::colour_bytes() const noexcept {
    // Four half-floats a pixel.
    return static_cast<u64>(desc_.width) * desc_.height * 8U;
}

Status FogOfWarPass::create_pipeline() noexcept {
    rhi::ShaderModuleBundle bundle;
    bundle.spirv = {kFogOfWarSpirv, sizeof(kFogOfWarSpirv) / sizeof(u32)};
    bundle.msl = {reinterpret_cast<const u8*>(kFogOfWarMsl), sizeof(kFogOfWarMsl) - 1};
    bundle.msl_entry_point = "cyFogOfWar";
    bundle.spirv_entry_point = "main";
    rhi::ValidationMessage message;
    auto module = rhi::select_shader_module(bundle, device_->capabilities().native_shader_format(),
                                            "fog of war", rhi::ShaderStage::Compute, message);
    if (!module.has_value()) {
        return make_unexpected(module.error());
    }
    Expected<rhi::ShaderModuleHandle, Error> shader = device_->create_shader_module(*module);
    if (!shader.has_value()) {
        return make_unexpected(shader.error());
    }
    shader_ = *shader;

    const rhi::DescriptorKind kinds[kBindings] = {
        rhi::DescriptorKind::SampledTexture, rhi::DescriptorKind::SampledTexture,
        rhi::DescriptorKind::SampledTexture, rhi::DescriptorKind::StorageBuffer,
        rhi::DescriptorKind::StorageTexture};
    rhi::DescriptorBinding bindings[kBindings] = {};
    for (u32 index = 0; index < kBindings; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = kinds[index];
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Compute;
    }
    rhi::DescriptorSetLayoutDescription set_layout;
    set_layout.name = "fog of war";
    set_layout.bindings = Span<const rhi::DescriptorBinding>(bindings, kBindings);
    Expected<rhi::DescriptorSetLayoutHandle, Error> layout =
        device_->create_descriptor_set_layout(set_layout);
    if (!layout.has_value()) {
        return make_unexpected(layout.error());
    }
    set_layout_ = *layout;

    rhi::PipelineLayoutDescription pipeline_layout;
    pipeline_layout.name = "fog of war";
    pipeline_layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(&set_layout_, 1);
    Expected<rhi::PipelineLayoutHandle, Error> made =
        device_->create_pipeline_layout(pipeline_layout);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    pipeline_layout_ = *made;

    rhi::ComputePipelineDescription pipeline;
    pipeline.name = "fog of war";
    pipeline.layout = pipeline_layout_;
    pipeline.shader = shader_;
    pipeline.workgroup_size[0] = kGroupSize;
    pipeline.workgroup_size[1] = kGroupSize;
    Expected<rhi::ComputePipelineHandle, Error> created =
        device_->create_compute_pipeline(pipeline);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    pipeline_ = *created;
    return ok();
}

Status FogOfWarPass::create_resources() noexcept {
    rhi::TextureDescription texture;
    texture.name = "fog of war visibility";
    texture.format = kVisibilityFormat;
    texture.extent = rhi::Extent3D{desc_.grid_width, desc_.grid_height, 1};
    texture.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination |
                    rhi::TextureUsage::TransferSource;
    Expected<rhi::TextureHandle, Error> made = device_->create_texture(texture);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    visibility_ = *made;

    const u32 slots = device_->frames_in_flight() < rhi::kMaxFramesInFlight
                          ? device_->frames_in_flight()
                          : rhi::kMaxFramesInFlight;
    for (u32 slot = 0; slot < slots; ++slot) {
        rhi::BufferDescription staging;
        staging.name = "fog of war texels";
        staging.size = texel_bytes();
        staging.usage = rhi::BufferUsage::TransferSource;
        staging.memory = rhi::MemoryUse::Upload;
        Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(staging);
        if (!buffer.has_value()) {
            return make_unexpected(buffer.error());
        }
        staging_[slot] = *buffer;

        rhi::BufferDescription words;
        words.name = "fog of war constants";
        words.size = kWordsBytes;
        words.usage = rhi::BufferUsage::Storage;
        words.memory = rhi::MemoryUse::Upload;
        buffer = device_->create_buffer(words);
        if (!buffer.has_value()) {
            return make_unexpected(buffer.error());
        }
        words_[slot] = *buffer;
    }

    if (device_->capabilities().has(rhi::Capability::TimestampQueries)) {
        rhi::QueryPoolDescription timer;
        timer.name = "fog of war timestamps";
        timer.kind = rhi::QueryKind::Timestamp;
        timer.count = 2;
        if (Expected<rhi::QueryPoolHandle, Error> pool = device_->create_query_pool(timer); pool) {
            timer_ = *pool;
        }
    }
    if (!desc_.readback) {
        return ok();
    }
    rhi::BufferDescription host;
    host.name = "fog of war visibility readback";
    host.size = texel_bytes();
    host.usage = rhi::BufferUsage::TransferDestination;
    host.memory = rhi::MemoryUse::Readback;
    Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(host);
    if (!buffer.has_value()) {
        return make_unexpected(buffer.error());
    }
    readback_ = *buffer;
    host.size = colour_bytes();
    host.name = "fog of war target readback";
    buffer = device_->create_buffer(host);
    if (!buffer.has_value()) {
        return make_unexpected(buffer.error());
    }
    readback_target_ = *buffer;
    return ok();
}

void FogOfWarPass::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    if (!visibility_.is_null()) {
        device_->destroy_texture(visibility_);
    }
    for (rhi::BufferHandle& buffer : staging_) {
        if (!buffer.is_null()) {
            device_->destroy_buffer(buffer);
        }
        buffer = {};
    }
    for (rhi::BufferHandle& buffer : words_) {
        if (!buffer.is_null()) {
            device_->destroy_buffer(buffer);
        }
        buffer = {};
    }
    for (rhi::BufferHandle* buffer : {&readback_, &readback_target_}) {
        if (!buffer->is_null()) {
            device_->destroy_buffer(*buffer);
        }
        *buffer = {};
    }
    if (timer_) {
        device_->destroy_query_pool(timer_);
    }
    timer_ = {};
    timed_ = false;
    if (!pipeline_.is_null()) {
        device_->destroy_compute_pipeline(pipeline_);
    }
    if (!pipeline_layout_.is_null()) {
        device_->destroy_pipeline_layout(pipeline_layout_);
    }
    if (!set_layout_.is_null()) {
        device_->destroy_descriptor_set_layout(set_layout_);
    }
    if (!shader_.is_null()) {
        device_->destroy_shader_module(shader_);
    }
    visibility_ = {};
    pipeline_ = {};
    pipeline_layout_ = {};
    set_layout_ = {};
    shader_ = {};
    texels_.clear();
    framed_ = false;
    report_ = FogOfWarReport{};
    device_ = nullptr;
}

Status FogOfWarPass::set_frame(const FogOfWarFrame& frame) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "fog of war: the pass is not created");
    }
    if (frame.field == nullptr || frame.field->width() != desc_.grid_width ||
        frame.field->height() != desc_.grid_height) {
        return fail(ErrorCode::InvalidArgument,
                    "fog of war: the frame's field is not the grid the pass was created for");
    }
    Expected<FogOfWarConstants, Error> constants =
        pack_fog_of_war_constants(frame.view, frame.placement, frame.style, desc_.width,
                                  desc_.height, desc_.grid_width, desc_.grid_height);
    if (!constants.has_value()) {
        return make_unexpected(constants.error());
    }
    const Span<const u8> texels = frame.field->texels();
    std::memcpy(texels_.data(), texels.data(), texels.size());
    constants_ = *constants;
    framed_ = true;
    return ok();
}

FrameStageDeclaration FogOfWarPass::stage() noexcept {
    return FrameStageDeclaration{&declare_stage, this};
}

PassId FogOfWarPass::declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                   void* user) noexcept {
    return static_cast<FogOfWarPass*>(user)->declare(graph, inputs);
}

PassId FogOfWarPass::declare(RenderGraph& graph, const ScreenSpaceStageInputs& inputs) noexcept {
    report_ = FogOfWarReport{};
    // Each refusal is a frame that would otherwise shade by a camera it was not given, or read an
    // image nothing wrote.
    if (device_ == nullptr || !framed_ || inputs.source == kInvalidResource ||
        inputs.target == kInvalidResource || inputs.depth == kInvalidResource ||
        inputs.width != desc_.width || inputs.height != desc_.height) {
        return kInvalidPass;
    }
    if (graph.resource(inputs.target).texture.format != kTargetFormat) {
        return kInvalidPass;
    }
    source_ = inputs.source;
    depth_ = inputs.depth;
    target_ = inputs.target;

    TextureRequest request;
    request.name = "fog of war visibility";
    request.format = kVisibilityFormat;
    request.width = desc_.grid_width;
    request.height = desc_.grid_height;
    request.extra_usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination |
                          rhi::TextureUsage::TransferSource;
    // UNDEFINED EVERY FRAME, and it is the truth: the upload writes every texel the dispatch reads.
    visibility_resource_ = graph.import_texture(request, visibility_, rhi::ImageUse::Undefined);

    const PassId first = graph.add_pass("fog of war upload", QueueKind::Graphics)
                             .write(visibility_resource_, Access::TransferWrite)
                             .record(&record_upload_step, this)
                             .id();
    graph.add_pass("fog of war", QueueKind::Graphics)
        .read(source_, Access::ComputeSampledRead)
        .read(depth_, Access::ComputeSampledRead)
        .read(visibility_resource_, Access::ComputeSampledRead)
        .write(target_, Access::ComputeStorageWrite)
        .record(&record_dispatch_step, this);
    report_.passes = 2;
    if (!desc_.readback) {
        return first;
    }
    BufferRequest host;
    host.name = "fog of war visibility readback";
    host.size = texel_bytes();
    host.extra_usage = rhi::BufferUsage::TransferDestination;
    readback_resource_ = graph.import_buffer(host, readback_);
    host.size = colour_bytes();
    host.name = "fog of war target readback";
    readback_target_resource_ = graph.import_buffer(host, readback_target_);
    graph.add_pass("fog of war readback", QueueKind::Graphics)
        .read(visibility_resource_, Access::TransferRead)
        .read(target_, Access::TransferRead)
        .write(readback_resource_, Access::TransferWrite)
        .write(readback_target_resource_, Access::TransferWrite)
        .record(&record_readback_step, this);
    // The host boundary as a declared read, so the graph derives the transfer-to-host barrier.
    graph.add_pass("fog of war host", QueueKind::Graphics)
        .read(readback_resource_, Access::HostRead)
        .read(readback_target_resource_, Access::HostRead)
        .side_effect();
    report_.passes += 2;
    return first;
}

void FogOfWarPass::record_upload(const PassContext& context) noexcept {
    // THE TEXELS ARE WRITTEN WHEN THE FRAME RECORDS, into the ring slot of the frame being
    // recorded: `begin_frame` has already waited for the frame that last used it.
    const u32 slot = device_->frame_slot() % rhi::kMaxFramesInFlight;
    const rhi::BufferHandle staging = staging_[slot];
    void* mapped = staging.is_null() ? nullptr : device_->buffer_mapped_pointer(staging);
    if (mapped == nullptr) {
        return;
    }
    std::memcpy(mapped, texels_.data(), texels_.size());
    if (timer_) {
        context.commands->reset_queries(timer_, 0, 2);
        context.commands->write_timestamp(timer_, 0);
    }
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{desc_.grid_width, desc_.grid_height, 1};
    context.commands->copy_buffer_to_texture(staging,
                                             context.executor->texture(visibility_resource_),
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
    report_.uploaded_bytes = texels_.size();
}

void FogOfWarPass::record_dispatch(const PassContext& context) noexcept {
    const u32 slot = device_->frame_slot() % rhi::kMaxFramesInFlight;
    const rhi::BufferHandle words = words_[slot];
    void* mapped = words.is_null() ? nullptr : device_->buffer_mapped_pointer(words);
    if (mapped == nullptr) {
        return;
    }
    std::memcpy(mapped, constants_.words, kWordsBytes);
    Expected<rhi::DescriptorSetHandle, Error> set =
        device_->allocate_descriptor_set(set_layout_, true);
    if (!set.has_value()) {
        return;
    }
    const GraphExecutor& executor = *context.executor;
    const ResourceId sampled[3] = {source_, depth_, visibility_resource_};
    rhi::DescriptorWrite writes[kBindings] = {};
    for (u32 index = 0; index < 3U; ++index) {
        writes[index].binding = index;
        writes[index].kind = rhi::DescriptorKind::SampledTexture;
        writes[index].texture_view = executor.view(sampled[index]);
        writes[index].use = rhi::ImageUse::SampledRead;
    }
    writes[3].binding = 3;
    writes[3].kind = rhi::DescriptorKind::StorageBuffer;
    writes[3].buffer = words;
    writes[3].buffer_range = kWordsBytes;
    writes[4].binding = 4;
    writes[4].kind = rhi::DescriptorKind::StorageTexture;
    writes[4].texture_view = executor.view(target_);
    writes[4].use = rhi::ImageUse::Storage;
    if (Status written = device_->update_descriptor_set(
            *set, Span<const rhi::DescriptorWrite>(writes, kBindings));
        !written) {
        return;
    }
    rhi::CommandBuffer& commands = *context.commands;
    commands.bind_compute_pipeline(pipeline_);
    commands.bind_descriptor_sets(pipeline_layout_, 0,
                                  Span<const rhi::DescriptorSetHandle>(&*set, 1));
    commands.dispatch(groups(desc_.width), groups(desc_.height), 1);
    if (timer_) {
        commands.write_timestamp(timer_, 1);
        timed_ = true;
    }
    report_.dispatched = true;
}

f64 FogOfWarPass::last_device_ms() const noexcept {
    if (device_ == nullptr || !timer_ || !timed_) {
        return -1.0;
    }
    u64 stamps[2] = {};
    Expected<u32, Error> read = device_->read_query_results(timer_, 0, 2, Span<u64>(stamps, 2));
    if (!read || *read != 2 || stamps[1] < stamps[0]) {
        return -1.0;
    }
    return static_cast<f64>(stamps[1] - stamps[0]) / 1.0e6;
}

void FogOfWarPass::record_readback(const PassContext& context) noexcept {
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{desc_.grid_width, desc_.grid_height, 1};
    const GraphExecutor& executor = *context.executor;
    context.commands->copy_texture_to_buffer(executor.texture(visibility_resource_), readback_,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
    region.texture_extent = rhi::Extent3D{desc_.width, desc_.height, 1};
    context.commands->copy_texture_to_buffer(executor.texture(target_), readback_target_,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

Status FogOfWarPass::read_back_visibility(Span<u8> out) const noexcept {
    if (device_ == nullptr || readback_.is_null()) {
        return fail(ErrorCode::Unavailable,
                    "fog of war: created without `readback`, so there is nothing to read");
    }
    if (out.size() != texel_bytes()) {
        return fail(ErrorCode::InvalidArgument,
                    "fog of war: the readback span is not the visibility texture's size");
    }
    const void* texels = device_->buffer_mapped_pointer(readback_);
    if (texels == nullptr) {
        return fail(ErrorCode::Internal, "fog of war: the readback buffer is not mapped");
    }
    std::memcpy(out.data(), texels, out.size());
    return ok();
}

Status FogOfWarPass::read_back_colour(Span<u16> target) const noexcept {
    if (device_ == nullptr || readback_target_.is_null()) {
        return fail(ErrorCode::Unavailable,
                    "fog of war: created without `readback`, so there is nothing to read");
    }
    if (target.size_bytes() != colour_bytes()) {
        return fail(ErrorCode::InvalidArgument,
                    "fog of war: the colour readback span is not the stage's extent");
    }
    const void* after = device_->buffer_mapped_pointer(readback_target_);
    if (after == nullptr) {
        return fail(ErrorCode::Internal, "fog of war: the readback buffer is not mapped");
    }
    std::memcpy(target.data(), after, target.size_bytes());
    return ok();
}

}  // namespace cy::rendering::fog_of_war
