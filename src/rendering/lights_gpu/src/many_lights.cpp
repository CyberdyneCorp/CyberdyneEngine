// SPDX-License-Identifier: MIT
// The stochastic many-light path on the device. See many_lights.h.

#include <cy/rendering/lights_gpu/many_lights.h>

#include <cy/backends/rhi/command_buffer.h>
#include <cy/core/math/matrix.h>
#include <cy/rendering/graph/executor.h>

#include <bit>
#include <cmath>
#include <cstring>

#include "lights_gpu_msl.h"
#include "lights_gpu_spirv.h"

namespace cy::rendering::lights_gpu {
namespace {

using rhi::Access;
using rhi::QueueKind;

constexpr u32 kGroupSize = 8;
constexpr f32 kPi = 3.14159265358979F;

/// many_lights.slang's `CyManyLightSet`, in its field order.
enum Binding : u32 {
    kWords = 0,
    kLights,
    kCandidates,
    kDepth,
    kNormals,
    kTemporalReservoir,
    kHistoryReservoir,
    kFinalReservoirBinding,
    kHistorySurface,
    kSurfaceBinding,
    kGuidance,
    kNoisy,
    kResolved,
    kHistoryColour,
    kHistoryMoments,
    kAccumulatedBinding,
    kMomentsBinding,
    kVariance,
    kFilterIn,
    kFilterOut,
    kTarget,
    kBindingCount,
};

constexpr rhi::DescriptorKind kBindingKinds[kBindingCount] = {
    rhi::DescriptorKind::StorageBuffer,  rhi::DescriptorKind::StorageBuffer,
    rhi::DescriptorKind::StorageBuffer,  rhi::DescriptorKind::SampledTexture,
    rhi::DescriptorKind::SampledTexture, rhi::DescriptorKind::StorageTexture,
    rhi::DescriptorKind::StorageTexture, rhi::DescriptorKind::StorageTexture,
    rhi::DescriptorKind::StorageTexture, rhi::DescriptorKind::StorageTexture,
    rhi::DescriptorKind::StorageTexture, rhi::DescriptorKind::StorageTexture,
    rhi::DescriptorKind::StorageTexture, rhi::DescriptorKind::StorageTexture,
    rhi::DescriptorKind::StorageTexture, rhi::DescriptorKind::StorageTexture,
    rhi::DescriptorKind::StorageTexture, rhi::DescriptorKind::StorageTexture,
    rhi::DescriptorKind::StorageTexture, rhi::DescriptorKind::StorageTexture,
    rhi::DescriptorKind::StorageTexture,
};

/// The steps of a frame, each one graph pass: the record callback's `step`.
enum Step : u32 {
    kStepCandidates = 0,
    kStepSpatial,
    kStepAccumulate,
    kStepVariance,
    kStepFilter0,
    kStepFilter1,
    kStepFilter2,
    kStepFilter3,
    kStepReadback,
    kStepCount,
};

/// The most a-trous passes the ladder declares: `quality_ladder()[0].max_passes`.
constexpr u32 kMaxFilterPasses = 4;

[[nodiscard]] u32 bits(f32 value) noexcept {
    return std::bit_cast<u32>(value);
}

[[nodiscard]] f32 as_float(u32 value) noexcept {
    return std::bit_cast<f32>(value);
}

[[nodiscard]] Mat4 from_rows(const f32 (&rows)[16]) noexcept {
    Mat4 matrix = Mat4::identity();
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            matrix.columns[column][row] = rows[(row * 4U) + column];
        }
    }
    return matrix;
}

[[nodiscard]] Vec4 row_of(const f32 (&rows)[16], u32 row) noexcept {
    return Vec4{rows[(row * 4U) + 0], rows[(row * 4U) + 1], rows[(row * 4U) + 2],
                rows[(row * 4U) + 3]};
}

[[nodiscard]] u32 groups(u32 extent) noexcept {
    return (extent + kGroupSize - 1U) / kGroupSize;
}

void destroy_buffer(rhi::Device& device, rhi::BufferHandle& buffer) noexcept {
    if (!buffer.is_null()) {
        device.destroy_buffer(buffer);
    }
    buffer = {};
}

}  // namespace

f32 light_power(const GpuLight& light) noexcept {
    const f32 luminance =
        (0.2126F * light.color[0]) + (0.7152F * light.color[1]) + (0.0722F * light.color[2]);
    const f32 half_x = light.spot_scale;
    const f32 half_y = light.spot_bias;
    const f32 sides = (light.kind & kGpuLightTwoSided) != 0U ? 2.0F : 1.0F;
    f32 power = 0.0F;
    switch (light.kind & kGpuLightKindMask) {
        case kGpuLightPoint:
            power = 4.0F * kPi * light.intensity;
            break;
        case kGpuLightSpot:
        case kGpuLightEmitter:
            power = kPi * light.intensity;
            break;
        case kGpuLightRect:
            power = kPi * light.intensity * 4.0F * half_x * half_y * sides;
            break;
        case kGpuLightDisc:
            power = kPi * light.intensity * kPi * half_x * half_x * sides;
            break;
        case kGpuLightSphere:
            power = kPi * light.intensity * 4.0F * kPi * half_x * half_x;
            break;
        case kGpuLightTube:
            // The side alone: a tube's ends do not emit.
            power = kPi * light.intensity * (2.0F * kPi * half_y * 2.0F * half_x);
            break;
        default:
            return 0.0F;
    }
    return power > 0.0F && luminance > 0.0F ? power * luminance : 0.0F;
}

Status build_alias_table(Span<const GpuLight> lights, Array<AliasEntry>& out) noexcept {
    out.clear();
    Array<f32> scaled(out.allocator());
    Array<u32> small(out.allocator());
    Array<u32> large(out.allocator());
    f32 total = 0.0F;
    for (usize index = 0; index < lights.size(); ++index) {
        const f32 power = light_power(lights[index]);
        if (power <= 0.0F) {
            continue;
        }
        AliasEntry entry;
        entry.light = static_cast<u32>(index);
        if (Status added = out.push_back(entry); !added) {
            return added;
        }
        if (Status added = scaled.push_back(power); !added) {
            return added;
        }
        total += power;
    }
    const auto count = static_cast<u32>(out.size());
    if (count == 0) {
        return ok();
    }
    // VOSE'S ALIAS METHOD: every slot holds its own light up to a threshold and another's above
    // it, so a draw is one uniform slot and one comparison, and each light's total probability is
    // its share of the power exactly.
    for (u32 slot = 0; slot < count; ++slot) {
        const f32 probability = scaled[slot] / total;
        out[slot].pdf = bits(probability);
        scaled[slot] = probability * static_cast<f32>(count);
        Status added = scaled[slot] < 1.0F ? small.push_back(slot) : large.push_back(slot);
        if (!added) {
            return added;
        }
    }
    while (!small.empty() && !large.empty()) {
        const u32 less = small.back();
        small.pop_back();
        const u32 more = large.back();
        large.pop_back();
        out[less].threshold = bits(scaled[less]);
        out[less].alias = more;
        scaled[more] = (scaled[more] + scaled[less]) - 1.0F;
        Status added = scaled[more] < 1.0F ? small.push_back(more) : large.push_back(more);
        if (!added) {
            return added;
        }
    }
    // What is left is one, up to rounding: kept whole.
    for (const u32 slot : large.span()) {
        out[slot].threshold = bits(1.0F);
        out[slot].alias = slot;
    }
    for (const u32 slot : small.span()) {
        out[slot].threshold = bits(1.0F);
        out[slot].alias = slot;
    }
    return ok();
}

u32 filter_pass_count(const ManyLightsSettings& settings) noexcept {
    const denoise::SignalConfig config =
        denoise::default_config(denoise::SignalKind::StochasticDirect);
    const Span<const denoise::QualityPosition> ladder = denoise::quality_ladder();
    const denoise::QualityPosition& quality =
        ladder[settings.quality_position < ladder.size() ? settings.quality_position
                                                         : ladder.size() - 1U];
    return config.max_passes < quality.max_passes ? config.max_passes : quality.max_passes;
}

Expected<ManyLightConstants, Error> pack_many_light_constants(const ManyLightsFrame& frame,
                                                              const ManyLightsSettings& settings,
                                                              u32 width, u32 height,
                                                              u32 candidate_count,
                                                              bool history_valid) noexcept {
    if (width == 0 || height == 0 || frame.view == nullptr) {
        return fail(ErrorCode::InvalidArgument, "many lights: no view, or an empty extent");
    }
    const pipeline::FrameViewData& view = *frame.view;
    Expected<Mat4, Error> inverse_clip = inverse(from_rows(view.relative_to_clip));
    if (!inverse_clip.has_value()) {
        return fail(ErrorCode::InvalidArgument, "many lights: the view's clip is singular");
    }
    const denoise::SignalConfig config =
        denoise::default_config(denoise::SignalKind::StochasticDirect);
    const Span<const denoise::QualityPosition> ladder = denoise::quality_ladder();
    const denoise::QualityPosition& quality =
        ladder[settings.quality_position < ladder.size() ? settings.quality_position
                                                         : ladder.size() - 1U];
    const u32 history_length = config.history_length < quality.history_length
                                   ? config.history_length
                                   : quality.history_length;
    const u32 passes =
        config.max_passes < quality.max_passes ? config.max_passes : quality.max_passes;

    ManyLightConstants constants;
    for (u32 column = 0; column < 4U; ++column) {
        constants.words[column] = inverse_clip->columns[column];
        constants.words[4U + column] = row_of(view.previous_relative_to_clip, column);
        constants.words[16U + column] = row_of(view.relative_to_clip, column);
    }
    constants.words[8] = Vec4{static_cast<f32>(width), static_cast<f32>(height),
                              1.0F / static_cast<f32>(width), 1.0F / static_cast<f32>(height)};
    constants.words[9] =
        Vec4{as_float(frame.frame_index), as_float(settings.path.candidates_per_pixel),
             as_float(candidate_count), as_float(settings.path.spatial_neighbours)};
    constants.words[10] =
        Vec4{static_cast<f32>(settings.path.max_history_samples), settings.spatial_radius_pixels,
             history_valid ? 1.0F : 0.0F, as_float(settings.visibility_steps)};
    constants.words[11] = row_of(view.relative_to_view, 2);
    constants.words[12] =
        Vec4{config.sigma_depth, config.sigma_normal, config.sigma_value, config.sigma_roughness};
    constants.words[13] = Vec4{static_cast<f32>(history_length), static_cast<f32>(passes),
                               static_cast<f32>(quality.kernel_extent),
                               history_length > 0 ? 1.0F / static_cast<f32>(history_length) : 1.0F};
    constants.words[14] = Vec4{settings.thickness_metres, settings.ray_offset_metres, 0.0F, 0.0F};
    constants.words[15] = Vec4{
        as_float(settings.temporal_reuse ? 1U : 0U), as_float(settings.spatial_reuse ? 1U : 0U),
        as_float(settings.visibility ? 1U : 0U), as_float(static_cast<u32>(frame.lights.size()))};
    return constants;
}

ManyLightsPass::~ManyLightsPass() {
    destroy();
}

bool ManyLightsPass::supported(const rhi::Device& device) noexcept {
    return detail::compute_supported(device);
}

Status ManyLightsPass::create(Allocator& allocator, rhi::Device& device,
                              const ManyLightsPassDescription& desc,
                              const ManyLightsSettings& settings) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the many-light pass is already created");
    }
    // THE CONFIGURATION FIRST: a path without a denoiser is refused on every device, before
    // anything asks what the device can do.
    if (Status valid = set_settings(settings); !valid) {
        return valid;
    }
    if (!supported(device)) {
        return fail(ErrorCode::Unsupported,
                    "the many-light path is compute dispatches and this device has no compute "
                    "stage or no shader format this module ships (SPIR-V or MSL)");
    }
    if (desc.width == 0 || desc.height == 0 || desc.max_lights == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "ManyLightsPassDescription's extent and light count must be non-zero");
    }
    device_ = &device;
    allocator_ = &allocator;
    desc_ = desc;
    if (Status made = create_pipelines(); !made) {
        destroy();
        return made;
    }
    if (Status made = create_textures(); !made) {
        destroy();
        return made;
    }
    if (Status made = create_buffers(); !made) {
        destroy();
        return made;
    }
    return ok();
}

Status ManyLightsPass::set_settings(const ManyLightsSettings& settings) noexcept {
    ManyLightSettings path = settings.path;
    // This pass IS the path, so it validates as enabled whatever the flag says: a pass created
    // without a denoiser is the configuration the requirement refuses.
    path.enabled = true;
    if (Status valid = validate_many_light(path); !valid) {
        return valid;
    }
    if (settings.visibility_steps == 0 || !(settings.thickness_metres > 0.0F)) {
        return fail(ErrorCode::InvalidArgument,
                    "many lights: a visibility ray needs at least one step and a thickness");
    }
    settings_ = settings;
    return ok();
}

Status ManyLightsPass::create_pipelines() noexcept {
    struct Entry {
        const char* name;
        Span<const u32> spirv;
        const char* msl;
        usize msl_size;
        const char* entry;
        detail::ComputePipeline* pipeline;
    };
    const Entry entries[] = {
        {"many lights candidates",
         {kManyLightsCandidatesSpirv, sizeof(kManyLightsCandidatesSpirv) / sizeof(u32)},
         kManyLightsCandidatesMsl,
         sizeof(kManyLightsCandidatesMsl),
         "cyManyLightsCandidates",
         &candidates_pipeline_},
        {"many lights spatial",
         {kManyLightsSpatialSpirv, sizeof(kManyLightsSpatialSpirv) / sizeof(u32)},
         kManyLightsSpatialMsl,
         sizeof(kManyLightsSpatialMsl),
         "cyManyLightsSpatial",
         &spatial_pipeline_},
        {"many lights accumulate",
         {kManyLightsAccumulateSpirv, sizeof(kManyLightsAccumulateSpirv) / sizeof(u32)},
         kManyLightsAccumulateMsl,
         sizeof(kManyLightsAccumulateMsl),
         "cyManyLightsAccumulate",
         &accumulate_pipeline_},
        {"many lights variance",
         {kManyLightsVarianceSpirv, sizeof(kManyLightsVarianceSpirv) / sizeof(u32)},
         kManyLightsVarianceMsl,
         sizeof(kManyLightsVarianceMsl),
         "cyManyLightsVariance",
         &variance_pipeline_},
        {"many lights filter",
         {kManyLightsFilterSpirv, sizeof(kManyLightsFilterSpirv) / sizeof(u32)},
         kManyLightsFilterMsl,
         sizeof(kManyLightsFilterMsl),
         "cyManyLightsFilter",
         &filter_pipeline_},
        {"many lights filter last",
         {kManyLightsFilterLastSpirv, sizeof(kManyLightsFilterLastSpirv) / sizeof(u32)},
         kManyLightsFilterLastMsl,
         sizeof(kManyLightsFilterLastMsl),
         "cyManyLightsFilterLast",
         &filter_last_pipeline_},
    };
    for (const Entry& entry : entries) {
        detail::ComputeProgram program;
        program.name = entry.name;
        program.spirv = entry.spirv;
        program.msl = {reinterpret_cast<const u8*>(entry.msl), entry.msl_size - 1};
        program.msl_entry = entry.entry;
        program.bindings = Span<const rhi::DescriptorKind>(kBindingKinds, kBindingCount);
        program.push_bytes = 16;
        program.workgroup[0] = kGroupSize;
        program.workgroup[1] = kGroupSize;
        if (Status made = entry.pipeline->create(*device_, program); !made) {
            return made;
        }
    }
    return ok();
}

Status ManyLightsPass::create_texture(const char* name, rhi::Format format, bool sampled,
                                      Texture& out) noexcept {
    rhi::TextureDescription description;
    description.name = name;
    description.format = format;
    description.extent = rhi::Extent3D{desc_.width, desc_.height, 1};
    description.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::TransferSource;
    if (sampled) {
        description.usage = description.usage | rhi::TextureUsage::Sampled;
    }
    Expected<rhi::TextureHandle, Error> texture = device_->create_texture(description);
    if (!texture.has_value()) {
        return make_unexpected(texture.error());
    }
    out.texture = *texture;
    rhi::TextureViewDescription view;
    view.name = name;
    view.texture = out.texture;
    Expected<rhi::TextureViewHandle, Error> made = device_->create_texture_view(view);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    out.view = *made;
    out.use = rhi::ImageUse::Undefined;
    return ok();
}

Status ManyLightsPass::create_textures() noexcept {
    const char* names[kPersistentCount] = {"many lights reservoirs", "many lights surface",
                                           "many lights accumulation", "many lights moments"};
    for (u32 kind = 0; kind < kPersistentCount; ++kind) {
        for (Texture& texture : persistent_[kind]) {
            if (Status made =
                    create_texture(names[kind], rhi::Format::Rgba32Sfloat, false, texture);
                !made) {
                return made;
            }
        }
    }
    if (Status made =
            create_texture("many lights irradiance", rhi::Format::Rgba16Sfloat, true, irradiance_);
        !made) {
        return made;
    }
    return create_texture("many lights resolved reservoir", rhi::Format::Rgba32Sfloat, true,
                          resolved_);
}

Status ManyLightsPass::create_buffers() noexcept {
    rhi::Device& device = *device_;
    const u32 slots = device.frames_in_flight() < rhi::kMaxFramesInFlight
                          ? device.frames_in_flight()
                          : rhi::kMaxFramesInFlight;
    for (u32 slot = 0; slot < slots; ++slot) {
        Expected<rhi::BufferHandle, Error> buffer = detail::host_buffer(
            device, "many lights constants", sizeof(ManyLightConstants), rhi::BufferUsage::Storage);
        if (!buffer.has_value()) {
            return make_unexpected(buffer.error());
        }
        words_[slot] = *buffer;
        buffer = detail::host_buffer(device, "many lights candidates",
                                     u64{desc_.max_lights} * sizeof(AliasEntry),
                                     rhi::BufferUsage::Storage);
        if (!buffer.has_value()) {
            return make_unexpected(buffer.error());
        }
        alias_[slot] = *buffer;
    }
    if (device.capabilities().has(rhi::Capability::TimestampQueries)) {
        rhi::QueryPoolDescription timer;
        timer.name = "many lights timestamps";
        timer.kind = rhi::QueryKind::Timestamp;
        timer.count = 2;
        if (Expected<rhi::QueryPoolHandle, Error> pool = device.create_query_pool(timer); pool) {
            timer_ = *pool;
        }
    }
    if (!desc_.readback) {
        return ok();
    }
    const u64 pixels = u64{desc_.width} * desc_.height;
    struct Readback {
        const char* name;
        u64 bytes;
        rhi::BufferHandle* buffer;
    };
    const Readback readbacks[] = {
        {"many lights noisy readback", pixels * 16U, &readback_noisy_},
        {"many lights guidance readback", pixels * 8U, &readback_guidance_},
        {"many lights surface readback", pixels * 16U, &readback_surface_},
        {"many lights irradiance readback", pixels * 8U, &readback_irradiance_},
        {"many lights resolved readback", pixels * 16U, &readback_resolved_},
    };
    for (const Readback& readback : readbacks) {
        rhi::BufferDescription host;
        host.name = readback.name;
        host.size = readback.bytes;
        host.usage = rhi::BufferUsage::TransferDestination;
        host.memory = rhi::MemoryUse::Readback;
        Expected<rhi::BufferHandle, Error> buffer = device.create_buffer(host);
        if (!buffer.has_value()) {
            return make_unexpected(buffer.error());
        }
        *readback.buffer = *buffer;
    }
    return ok();
}

void ManyLightsPass::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    rhi::Device& device = *device_;
    (void)device.wait_idle();
    const auto release = [&](Texture& texture) noexcept {
        if (!texture.view.is_null()) {
            device.destroy_texture_view(texture.view);
        }
        if (!texture.texture.is_null()) {
            device.destroy_texture(texture.texture);
        }
        texture = Texture{};
    };
    for (auto& pair : persistent_) {
        release(pair[0]);
        release(pair[1]);
    }
    release(irradiance_);
    release(resolved_);
    for (u32 slot = 0; slot < rhi::kMaxFramesInFlight; ++slot) {
        destroy_buffer(device, words_[slot]);
        destroy_buffer(device, alias_[slot]);
    }
    for (rhi::BufferHandle* buffer : {&readback_noisy_, &readback_guidance_, &readback_surface_,
                                      &readback_irradiance_, &readback_resolved_}) {
        destroy_buffer(device, *buffer);
    }
    if (timer_) {
        device.destroy_query_pool(timer_);
    }
    timer_ = {};
    timed_ = false;
    for (detail::ComputePipeline* pipeline :
         {&candidates_pipeline_, &spatial_pipeline_, &accumulate_pipeline_, &variance_pipeline_,
          &filter_pipeline_, &filter_last_pipeline_}) {
        pipeline->destroy(device);
    }
    candidates_.clear();
    framed_ = false;
    history_valid_ = false;
    declared_ = false;
    parity_ = 0;
    report_ = ManyLightsReport{};
    device_ = nullptr;
    allocator_ = nullptr;
}

ManyLightsTargets ManyLightsPass::import_targets(RenderGraph& graph) noexcept {
    ManyLightsTargets targets;
    if (device_ == nullptr) {
        return targets;
    }
    TextureRequest request;
    request.width = desc_.width;
    request.height = desc_.height;
    request.extra_usage =
        rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferSource;
    request.name = "many lights irradiance";
    request.format = rhi::Format::Rgba16Sfloat;
    irradiance_id_ = graph.import_texture(request, irradiance_.texture, irradiance_.use);
    request.name = "many lights resolved reservoir";
    request.format = rhi::Format::Rgba32Sfloat;
    resolved_id_ = graph.import_texture(request, resolved_.texture, resolved_.use);
    targets.irradiance = irradiance_id_;
    targets.reservoir = resolved_id_;
    return targets;
}

pipeline::MaterialTextureSlot ManyLightsPass::irradiance_slot(
    rhi::BindlessIndex index) const noexcept {
    return pipeline::MaterialTextureSlot{index, irradiance_.view};
}

pipeline::MaterialTextureSlot ManyLightsPass::reservoir_slot(
    rhi::BindlessIndex index) const noexcept {
    return pipeline::MaterialTextureSlot{index, resolved_.view};
}

Status ManyLightsPass::set_frame(const ManyLightsFrame& frame) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::Unavailable, "many lights: create() was not called");
    }
    if (frame.lights.size() > desc_.max_lights) {
        return fail(ErrorCode::OutOfRange,
                    "many lights: more lights than ManyLightsPassDescription::max_lights");
    }

    if (Status built = build_alias_table(frame.lights, candidates_); !built) {
        return built;
    }
    const bool history = history_valid_ && !frame.history_invalid;
    Expected<ManyLightConstants, Error> constants = pack_many_light_constants(
        frame, settings_, desc_.width, desc_.height, static_cast<u32>(candidates_.size()), history);
    if (!constants.has_value()) {
        return make_unexpected(constants.error());
    }
    constants_ = *constants;
    light_buffer_ = frame.light_buffer;
    report_.candidate_lights = static_cast<u32>(candidates_.size());
    ManyLightSettings path = settings_.path;
    path.enabled = true;
    report_.stats = lighting_path_stats(path, 0, 0);
    framed_ = true;
    return ok();
}

FrameStageDeclaration ManyLightsPass::stage() noexcept {
    return FrameStageDeclaration{&declare_stage, this, &executed_stage};
}

PassId ManyLightsPass::declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                     void* user) noexcept {
    return static_cast<ManyLightsPass*>(user)->declare(graph, inputs);
}

void ManyLightsPass::executed_stage(void* user) noexcept {
    static_cast<ManyLightsPass*>(user)->executed();
}

namespace {

/// The record callbacks, one per step: `RecordFn` carries one user pointer — the pass — so the
/// step is the function.
template <u32 kStep>
void record_step_fn(const PassContext& context, void* user) noexcept {
    static_cast<ManyLightsPass*>(user)->record_step(context, kStep);
}

constexpr RecordFn kRecorders[kStepCount] = {
    &record_step_fn<kStepCandidates>, &record_step_fn<kStepSpatial>,
    &record_step_fn<kStepAccumulate>, &record_step_fn<kStepVariance>,
    &record_step_fn<kStepFilter0>,    &record_step_fn<kStepFilter1>,
    &record_step_fn<kStepFilter2>,    &record_step_fn<kStepFilter3>,
    &record_step_fn<kStepReadback>,
};

}  // namespace

PassId ManyLightsPass::declare(RenderGraph& graph, const ScreenSpaceStageInputs& inputs) noexcept {
    report_.passes = 0;
    report_.dispatched = false;
    declared_ = false;
    if (device_ == nullptr || inputs.depth == kInvalidResource ||
        inputs.normal_roughness == kInvalidResource || inputs.target != irradiance_id_ ||
        irradiance_id_ == kInvalidResource || inputs.width != desc_.width ||
        inputs.height != desc_.height) {
        return kInvalidPass;
    }
    depth_ = inputs.depth;
    normals_ = inputs.normal_roughness;
    lights_ = inputs.lights;
    const u32 write = parity_;
    const u32 read = parity_ ^ 1U;
    const char* names[kPersistentCount] = {"many lights reservoirs", "many lights surface",
                                           "many lights accumulation", "many lights moments"};
    TextureRequest request;
    request.width = desc_.width;
    request.height = desc_.height;
    request.format = rhi::Format::Rgba32Sfloat;
    request.extra_usage = rhi::TextureUsage::Storage | rhi::TextureUsage::TransferSource;
    for (u32 kind = 0; kind < kPersistentCount; ++kind) {
        request.name = names[kind];
        for (u32 half = 0; half < 2U; ++half) {
            Texture& texture = persistent_[kind][half];
            persistent_ids_[kind][half] =
                graph.import_texture(request, texture.texture, texture.use);
        }
    }
    request.extra_usage = rhi::TextureUsage::Storage;
    request.name = "many lights temporal reservoirs";
    temporal_reservoir_ = graph.create_texture(request);
    request.name = "many lights noisy";
    noisy_ = graph.create_texture(request);
    request.name = "many lights filter ping";
    ping_ = graph.create_texture(request);
    request.name = "many lights filter pong";
    pong_ = graph.create_texture(request);
    request.name = "many lights guidance";
    request.format = rhi::Format::Rg32Sfloat;
    guidance_ = graph.create_texture(request);
    request.name = "many lights variance";
    request.format = rhi::Format::R32Sfloat;
    variance_ = graph.create_texture(request);

    const ResourceId reservoir_read = persistent_ids_[kFinalReservoir][read];
    const ResourceId reservoir_write = persistent_ids_[kFinalReservoir][write];
    const ResourceId surface_read = persistent_ids_[kSurface][read];
    const ResourceId surface_write = persistent_ids_[kSurface][write];
    const ResourceId colour_read = persistent_ids_[kAccumulated][read];
    const ResourceId colour_write = persistent_ids_[kAccumulated][write];
    const ResourceId moments_read = persistent_ids_[kMoments][read];
    const ResourceId moments_write = persistent_ids_[kMoments][write];

    // THE LIGHTS ARE READ FROM THE FRAME'S DEVICE COPY, the one its prepare stage fills, when the
    // frame has one: a read across the bus from the host's ring for every candidate of every pixel
    // is the cost this path exists to avoid.
    PassBuilder candidates = graph.add_pass("many lights candidates", QueueKind::Graphics);
    candidates.read(depth_, Access::ComputeSampledRead)
        .read(normals_, Access::ComputeSampledRead)
        .read(surface_read, Access::ComputeStorageRead)
        .read(reservoir_read, Access::ComputeStorageRead)
        .write(surface_write, Access::ComputeStorageWrite)
        .write(guidance_, Access::ComputeStorageWrite)
        .write(temporal_reservoir_, Access::ComputeStorageWrite);
    if (lights_ != kInvalidResource) {
        candidates.read(lights_, Access::ComputeStorageRead);
    }
    const PassId first = candidates.record(kRecorders[kStepCandidates], this).id();
    PassBuilder spatial = graph.add_pass("many lights spatial", QueueKind::Graphics);
    spatial.read(depth_, Access::ComputeSampledRead)
        .read(normals_, Access::ComputeSampledRead)
        .read(surface_write, Access::ComputeStorageRead)
        .read(temporal_reservoir_, Access::ComputeStorageRead)
        .write(reservoir_write, Access::ComputeStorageWrite)
        .write(noisy_, Access::ComputeStorageWrite)
        .write(resolved_id_, Access::ComputeStorageWrite);
    if (lights_ != kInvalidResource) {
        spatial.read(lights_, Access::ComputeStorageRead);
    }
    spatial.record(kRecorders[kStepSpatial], this);
    graph.add_pass("many lights accumulate", QueueKind::Graphics)
        .read(noisy_, Access::ComputeStorageRead)
        .read(guidance_, Access::ComputeStorageRead)
        .read(colour_read, Access::ComputeStorageRead)
        .read(moments_read, Access::ComputeStorageRead)
        .write(colour_write, Access::ComputeStorageWrite)
        .write(moments_write, Access::ComputeStorageWrite)
        .record(kRecorders[kStepAccumulate], this);
    graph.add_pass("many lights variance", QueueKind::Graphics)
        .read(moments_write, Access::ComputeStorageRead)
        .write(variance_, Access::ComputeStorageWrite)
        .record(kRecorders[kStepVariance], this);
    const u32 passes = filter_pass_count(settings_);
    const u32 filter_passes =
        passes == 0U ? 1U : (passes > kMaxFilterPasses ? kMaxFilterPasses : passes);
    ResourceId input = colour_write;
    for (u32 pass = 0; pass < filter_passes; ++pass) {
        const bool last = pass + 1U == filter_passes;
        const ResourceId output = last ? irradiance_id_ : ((pass & 1U) == 0U ? ping_ : pong_);
        graph.add_pass(last ? "many lights filter last" : "many lights filter", QueueKind::Graphics)
            .read(input, Access::ComputeStorageRead)
            .read(surface_write, Access::ComputeStorageRead)
            .read(variance_, Access::ComputeStorageRead)
            .read(moments_write, Access::ComputeStorageRead)
            .write(output, Access::ComputeStorageWrite)
            .record(kRecorders[kStepFilter0 + pass], this);
        input = output;
    }
    filter_passes_ = filter_passes;
    report_.passes = 4U + filter_passes;
    if (desc_.readback) {
        BufferRequest host;
        host.extra_usage = rhi::BufferUsage::TransferDestination;
        const u64 pixels = u64{desc_.width} * desc_.height;
        const rhi::BufferHandle buffers[5] = {readback_noisy_, readback_guidance_,
                                              readback_surface_, readback_irradiance_,
                                              readback_resolved_};
        const u64 sizes[5] = {pixels * 16U, pixels * 8U, pixels * 16U, pixels * 8U, pixels * 16U};
        for (u32 index = 0; index < 5U; ++index) {
            host.name = "many lights readback";
            host.size = sizes[index];
            readback_ids_[index] = graph.import_buffer(host, buffers[index]);
        }
        PassBuilder readback = graph.add_pass("many lights readback", QueueKind::Graphics);
        readback.read(noisy_, Access::TransferRead)
            .read(guidance_, Access::TransferRead)
            .read(surface_write, Access::TransferRead)
            .read(irradiance_id_, Access::TransferRead)
            .read(resolved_id_, Access::TransferRead);
        for (const ResourceId id : readback_ids_) {
            readback.write(id, Access::TransferWrite);
        }
        readback.record(kRecorders[kStepReadback], this);
        PassBuilder host_pass = graph.add_pass("many lights host", QueueKind::Graphics);
        for (const ResourceId id : readback_ids_) {
            host_pass.read(id, Access::HostRead);
        }
        host_pass.side_effect();
        report_.passes += 2;
    }
    declared_ = true;
    return first;
}

void ManyLightsPass::dispatch(const PassContext& context, const detail::ComputePipeline& pipeline,
                              u32 filter_pass, rhi::TextureViewHandle filter_in,
                              rhi::TextureViewHandle filter_out) noexcept {
    rhi::Device& device = *device_;
    const GraphExecutor& executor = *context.executor;
    const u32 slot = device.frame_slot() % rhi::kMaxFramesInFlight;
    const u32 write = parity_;
    const u32 read = parity_ ^ 1U;
    const auto view_of = [&](u32 kind, u32 half) noexcept {
        return executor.view(persistent_ids_[kind][half]);
    };
    rhi::DescriptorWrite writes[kBindingCount] = {
        detail::storage_write(kWords, words_[slot]),
        detail::storage_write(kLights, lights_ != kInvalidResource ? executor.buffer(lights_)
                                       : light_buffer_.is_null()   ? words_[slot]
                                                                   : light_buffer_),
        detail::storage_write(kCandidates, alias_[slot]),
        detail::texture_write(kDepth, executor.view(depth_), false),
        detail::texture_write(kNormals, executor.view(normals_), false),
        detail::texture_write(kTemporalReservoir, executor.view(temporal_reservoir_), true),
        detail::texture_write(kHistoryReservoir, view_of(kFinalReservoir, read), true),
        detail::texture_write(kFinalReservoirBinding, view_of(kFinalReservoir, write), true),
        detail::texture_write(kHistorySurface, view_of(kSurface, read), true),
        detail::texture_write(kSurfaceBinding, view_of(kSurface, write), true),
        detail::texture_write(kGuidance, executor.view(guidance_), true),
        detail::texture_write(kNoisy, executor.view(noisy_), true),
        detail::texture_write(kResolved, executor.view(resolved_id_), true),
        detail::texture_write(kHistoryColour, view_of(kAccumulated, read), true),
        detail::texture_write(kHistoryMoments, view_of(kMoments, read), true),
        detail::texture_write(kAccumulatedBinding, view_of(kAccumulated, write), true),
        detail::texture_write(kMomentsBinding, view_of(kMoments, write), true),
        detail::texture_write(kVariance, executor.view(variance_), true),
        detail::texture_write(kFilterIn, filter_in, true),
        detail::texture_write(kFilterOut, filter_out, true),
        detail::texture_write(kTarget, executor.view(irradiance_id_), true),
    };
    // The depth is a depth texture; its sampled view reads depth, as the GTAO and the fog do.
    writes[kDepth].use = rhi::ImageUse::SampledRead;
    Expected<rhi::DescriptorSetHandle, Error> set =
        pipeline.set(device, Span<const rhi::DescriptorWrite>(writes, kBindingCount));
    if (!set.has_value()) {
        return;
    }
    rhi::CommandBuffer& commands = *context.commands;
    commands.bind_compute_pipeline(pipeline.pipeline());
    commands.bind_descriptor_sets(pipeline.layout(), 0,
                                  Span<const rhi::DescriptorSetHandle>(&*set, 1));
    const u32 push[4] = {filter_pass, 0, 0, 0};
    commands.push_constants(pipeline.layout(), rhi::ShaderStage::Compute, 0,
                            Span<const u8>(reinterpret_cast<const u8*>(push), sizeof(push)));
    commands.dispatch(groups(desc_.width), groups(desc_.height), 1);
}

void ManyLightsPass::record_step(const PassContext& context, u32 step) noexcept {
    rhi::Device& device = *device_;
    const GraphExecutor& executor = *context.executor;
    rhi::CommandBuffer& commands = *context.commands;
    const u32 passes = filter_passes_;
    if (!framed_) {
        // No `set_frame` between this frame's declaration and its recording: nothing to shade
        // with, and the targets keep what they held.
        return;
    }
    if (step == kStepCandidates) {
        // WRITTEN WHEN THE FRAME RECORDS, into the ring slot of the frame being recorded.
        const u32 slot = device.frame_slot() % rhi::kMaxFramesInFlight;
        void* words = device.buffer_mapped_pointer(words_[slot]);
        void* alias = device.buffer_mapped_pointer(alias_[slot]);
        if (words == nullptr || alias == nullptr) {
            return;
        }
        std::memcpy(words, &constants_, sizeof(constants_));
        if (!candidates_.empty()) {
            std::memcpy(alias, candidates_.data(), candidates_.size() * sizeof(AliasEntry));
        }
        if (timer_) {
            commands.reset_queries(timer_, 0, 2);
            commands.write_timestamp(timer_, 0);
        }
    }
    const rhi::TextureViewHandle ping = executor.view(ping_);
    const rhi::TextureViewHandle pong = executor.view(pong_);
    const rhi::TextureViewHandle accumulated =
        executor.view(persistent_ids_[kAccumulated][parity_]);
    switch (step) {
        case kStepCandidates:
            dispatch(context, candidates_pipeline_, 0, ping, pong);
            return;
        case kStepSpatial:
            dispatch(context, spatial_pipeline_, 0, ping, pong);
            return;
        case kStepAccumulate:
            dispatch(context, accumulate_pipeline_, 0, ping, pong);
            return;
        case kStepVariance:
            dispatch(context, variance_pipeline_, 0, ping, pong);
            return;
        case kStepReadback:
            break;
        default: {
            const u32 pass = step - kStepFilter0;
            const bool last = pass + 1U == passes;
            const rhi::TextureViewHandle in =
                pass == 0U ? accumulated : ((pass & 1U) == 1U ? ping : pong);
            const rhi::TextureViewHandle out = (pass & 1U) == 0U ? ping : pong;
            dispatch(context, last ? filter_last_pipeline_ : filter_pipeline_, pass, in, out);
            if (last) {
                if (timer_) {
                    commands.write_timestamp(timer_, 1);
                    timed_ = true;
                }
                report_.dispatched = true;
            }
            return;
        }
    }
    // The readback: five images into five host buffers.
    const ResourceId sources[5] = {noisy_, guidance_, persistent_ids_[kSurface][parity_],
                                   irradiance_id_, resolved_id_};
    const rhi::BufferHandle targets[5] = {readback_noisy_, readback_guidance_, readback_surface_,
                                          readback_irradiance_, readback_resolved_};
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{desc_.width, desc_.height, 1};
    for (u32 index = 0; index < 5U; ++index) {
        commands.copy_texture_to_buffer(executor.texture(sources[index]), targets[index],
                                        Span<const rhi::BufferTextureCopy>(&region, 1));
    }
}

void ManyLightsPass::executed() noexcept {
    if (!declared_) {
        return;
    }
    // Every persistent image was last written or read as storage, and both targets were sampled
    // by the opaque pass — or copied out, with a readback.
    for (auto& pair : persistent_) {
        pair[0].use = rhi::ImageUse::Storage;
        pair[1].use = rhi::ImageUse::Storage;
    }
    if (desc_.readback) {
        persistent_[kSurface][parity_].use = rhi::ImageUse::TransferSource;
    }
    irradiance_.use = rhi::ImageUse::SampledRead;
    resolved_.use = rhi::ImageUse::SampledRead;
    history_valid_ = framed_;
    parity_ ^= 1U;
    declared_ = false;
    framed_ = false;
}

f64 ManyLightsPass::last_device_ms() const noexcept {
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

namespace {

[[nodiscard]] f32 half_to_float(u16 half) noexcept {
    const u32 sign = (static_cast<u32>(half) & 0x8000U) << 16U;
    const u32 exponent = (static_cast<u32>(half) >> 10U) & 0x1FU;
    const u32 mantissa = static_cast<u32>(half) & 0x3FFU;
    if (exponent == 0U) {
        const f32 value = std::ldexp(static_cast<f32>(mantissa), -24);
        return sign != 0U ? -value : value;
    }
    if (exponent == 31U) {
        return std::bit_cast<f32>(sign | 0x7F800000U | (mantissa << 13U));
    }
    return std::bit_cast<f32>(sign | ((exponent + 112U) << 23U) | (mantissa << 13U));
}

}  // namespace

Status ManyLightsPass::read_back(ManyLightsReadback& out) const noexcept {
    if (device_ == nullptr || readback_noisy_.is_null()) {
        return fail(ErrorCode::Unavailable,
                    "many lights: created without `readback`, so there is nothing to read");
    }
    const usize pixels = static_cast<usize>(desc_.width) * desc_.height;
    for (Array<Vec4>* target : {&out.noisy, &out.surface, &out.irradiance, &out.resolved}) {
        if (Status sized = target->resize(pixels); !sized) {
            return sized;
        }
    }
    if (Status sized = out.guidance.resize(pixels * 2U); !sized) {
        return sized;
    }
    const auto* noisy = static_cast<const f32*>(device_->buffer_mapped_pointer(readback_noisy_));
    const auto* guidance =
        static_cast<const f32*>(device_->buffer_mapped_pointer(readback_guidance_));
    const auto* surface =
        static_cast<const f32*>(device_->buffer_mapped_pointer(readback_surface_));
    const auto* irradiance =
        static_cast<const u16*>(device_->buffer_mapped_pointer(readback_irradiance_));
    const auto* resolved =
        static_cast<const f32*>(device_->buffer_mapped_pointer(readback_resolved_));
    if (noisy == nullptr || guidance == nullptr || surface == nullptr || irradiance == nullptr ||
        resolved == nullptr) {
        return fail(ErrorCode::Internal, "many lights: a readback buffer is not mapped");
    }
    for (usize pixel = 0; pixel < pixels; ++pixel) {
        const usize four = pixel * 4U;
        out.noisy[pixel] = Vec4{noisy[four], noisy[four + 1], noisy[four + 2], noisy[four + 3]};
        out.surface[pixel] =
            Vec4{surface[four], surface[four + 1], surface[four + 2], surface[four + 3]};
        out.resolved[pixel] =
            Vec4{resolved[four], resolved[four + 1], resolved[four + 2], resolved[four + 3]};
        out.irradiance[pixel] =
            Vec4{half_to_float(irradiance[four]), half_to_float(irradiance[four + 1]),
                 half_to_float(irradiance[four + 2]), half_to_float(irradiance[four + 3])};
        out.guidance[pixel * 2U] = guidance[pixel * 2U];
        out.guidance[(pixel * 2U) + 1U] = guidance[(pixel * 2U) + 1U];
    }
    return ok();
}

}  // namespace cy::rendering::lights_gpu
