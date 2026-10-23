// SPDX-License-Identifier: MIT
#pragma once
// The stochastic many-light path on the device. `rendering-lighting-and-shadows` — "Stochastic
// many-light direct lighting": "candidate generation from the GPU light set, reservoir sampling
// with temporal and spatial reuse, ray-traced or shadow-map visibility for the selected samples,
// and denoising through `denoising`".
//
// ================================================================================================
// THE SHAPE
// ================================================================================================
//
//   depth, normals ─► candidates + temporal reuse ─► spatial reuse + visibility ─► noisy E·W·V
//                         ▲  last frame's reservoirs ◄────────────── final reservoirs
//   noisy ─► accumulate ─► variance ─► a-trous × passes ─► irradiance target ──┐
//                ▲ last frame's accumulation                                   ├─► opaque pass
//   final reservoir ─► resolved (light, W·V) target ───────────────────────────┘
//
// Declared at `FramePassKind::ManyLights` through `FrameStageDeclaration`, after the screen-space
// passes and before the opaque pass. The opaque pass shades every directional light as it always
// does and, for every LOCAL light, reads the two targets instead of walking its cluster: the
// diffuse half as `albedo / pi * E` from the denoised irradiance, the specular half as the
// resolved light shaded with the material and weighted (`cy/frame.slang`'s
// `frameStochasticLighting`).
//
// ================================================================================================
// WHAT IS SAMPLED, AND WHY THE ESTIMATE IS OF THE SHADOWED SUM
// ================================================================================================
//
// The source distribution is the lights' POWER (`light_power`): an alias table over the frame's
// local lights the host builds once a frame, so a draw is two random numbers and two reads. The
// target is the unshadowed irradiance each light would deliver here — exact for an area light, its
// form factor, as the forward pass shades it — so the reservoir's light is drawn in proportion to
// what it would add, and the estimate `E_y W V`, with `W = weight_sum / (M p(y))` and `V` one ray
// to a point drawn over the light's own surface, is unbiased for the sum over every light of its
// irradiance times the fraction of its surface the point sees. That is a soft shadow, and an area
// light's penumbra is its own size.
//
// VISIBILITY IS A RAY MARCHED THROUGH THE DEPTH BUFFER — ray-traced visibility against what the
// screen holds. An occluder off the screen casts no shadow on this path; `render.many_lights`
// states which scenes it draws. A traced ray through `ray-tracing-infrastructure`'s top level is
// the replacement, behind the same `traceVisibility`.
//
// ================================================================================================
// THE DENOISER IS THE ENGINE'S, TRANSCRIBED
// ================================================================================================
//
// `denoise::Denoiser` is the host's one filter for every stochastic signal, and this path is its
// `StochasticDirect` signal: `default_config(StochasticDirect)` and the quality ladder's position
// arrive as words, and the three dispatches are `accumulate`, `estimate_variance` and
// `filter_pixel` in their order. `render.many_lights` runs `Denoiser::denoise` over the device's
// own noisy signal and reprojection and compares.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/denoise/denoiser.h>
#include <cy/rendering/forward/frame.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/lighting/lights.h>
#include <cy/rendering/lighting/many_light.h>
#include <cy/rendering/lights_gpu/detail/compute.h>
#include <cy/rendering/pipeline/frame_pipelines.h>

namespace cy::rendering::lights_gpu {

/// many_lights.slang's words, `pack_many_light_constants`' layout.
inline constexpr u32 kManyLightConstantWords = 20;

struct ManyLightConstants {
    Vec4 words[kManyLightConstantWords] = {};
};

/// One entry of the alias table the candidates are drawn from: many_lights.slang's `uint4`.
struct AliasEntry {
    u32 light = 0;
    u32 alias = 0;
    /// `f32` bits: below it a draw keeps this slot, above it takes `alias`.
    u32 threshold = 0;
    /// `f32` bits: the probability this slot's light is drawn with.
    u32 pdf = 0;
};

static_assert(sizeof(AliasEntry) == 16);

/// A light's power, in the units its record carries: what the candidates are drawn in proportion
/// to. Zero for a directional light and a baked placeholder, which are never candidates.
[[nodiscard]] f32 light_power(const GpuLight& light) noexcept;

/// The alias table over every local light of `lights`, drawn in proportion to `light_power` —
/// Vose's construction, so a draw is one slot and one comparison. Empty when no light has power.
[[nodiscard]] Status build_alias_table(Span<const GpuLight> lights,
                                       Array<AliasEntry>& out) noexcept;

/// What the path does beyond `ManyLightSettings`: the reuse and the visibility ray.
struct ManyLightsSettings {
    /// Candidates, the spatial neighbours, the history cap and the denoiser requirement. Refused by
    /// `validate_many_light` when it is.
    ManyLightSettings path{};
    bool temporal_reuse = true;
    bool spatial_reuse = true;
    /// Off, every selected light is taken as visible: what the unbiasedness case measures against
    /// an unshadowed sum.
    bool visibility = true;
    f32 spatial_radius_pixels = 24.0F;
    u32 visibility_steps = 16;
    /// How far behind a depth sample a ray may pass and still be occluded by it, in metres.
    f32 thickness_metres = 0.5F;
    /// How far off the surface along its normal a visibility ray starts, in metres.
    f32 ray_offset_metres = 0.02F;
    /// The denoiser's ladder position (`denoise::quality_ladder`).
    u32 quality_position = 0;
};

struct ManyLightsPassDescription {
    u32 width = 0;
    u32 height = 0;
    u32 max_lights = 4096;
    /// Host copies of every intermediate after each frame, for a test.
    bool readback = false;
};

/// One frame's inputs.
struct ManyLightsFrame {
    /// The frame's lights, as uploaded. The dispatches read them from the frame's device copy —
    /// `ScreenSpaceStageInputs::lights`, which the prepare stage fills — and from `light_buffer`,
    /// a buffer of the same records, only for a frame that declares none.
    Span<const GpuLight> lights;
    rhi::BufferHandle light_buffer;
    /// The view block as uploaded: its jittered clip, last frame's clip about this camera, and its
    /// rotation.
    const pipeline::FrameViewData* view = nullptr;
    u32 frame_index = 0;
    /// Last frame's reservoirs and accumulation mean nothing: a cut, a resize, the first frame.
    bool history_invalid = false;
};

/// The a-trous passes the path's denoiser runs: `default_config(StochasticDirect)`'s cascade,
/// capped by the quality ladder's position.
[[nodiscard]] u32 filter_pass_count(const ManyLightsSettings& settings) noexcept;

/// The words the dispatches read. Refused when the view is singular or the extent zero.
[[nodiscard]] Expected<ManyLightConstants, Error> pack_many_light_constants(
    const ManyLightsFrame& frame, const ManyLightsSettings& settings, u32 width, u32 height,
    u32 candidate_count, bool history_valid) noexcept;

/// Host copies of one frame's intermediates, row-major from the top-left, a pixel each.
struct ManyLightsReadback {
    /// `E_y W V` — the noisy signal.
    Array<Vec4> noisy{system_allocator(MemoryDomain::Renderer)};
    /// (source pixel or -1, confidence): the reprojection.
    Array<f32> guidance{system_allocator(MemoryDomain::Renderer)};
    /// (view depth, normal): the surface.
    Array<Vec4> surface{system_allocator(MemoryDomain::Renderer)};
    /// The denoised irradiance, `rgba16f` as floats.
    Array<Vec4> irradiance{system_allocator(MemoryDomain::Renderer)};
    /// (light, W V): the resolved reservoir.
    Array<Vec4> resolved{system_allocator(MemoryDomain::Renderer)};
};

struct ManyLightsReport {
    u32 passes = 0;
    u32 candidate_lights = 0;
    bool dispatched = false;
    /// What `lighting_path_stats` reports for this path: the per-pixel bounds that replace the
    /// per-cluster one.
    LightingPathStats stats{};
};

/// The two targets the opaque pass samples, imported into a frame's graph.
struct ManyLightsTargets {
    ResourceId irradiance = kInvalidResource;
    ResourceId reservoir = kInvalidResource;
};

class ManyLightsPass {
public:
    ManyLightsPass() noexcept = default;
    ~ManyLightsPass();

    ManyLightsPass(const ManyLightsPass&) = delete;
    ManyLightsPass& operator=(const ManyLightsPass&) = delete;
    ManyLightsPass(ManyLightsPass&&) = delete;
    ManyLightsPass& operator=(ManyLightsPass&&) = delete;

    [[nodiscard]] static bool supported(const rhi::Device& device) noexcept;

    /// Refuses settings `validate_many_light` refuses: "This path SHALL NOT be enabled without
    /// denoising ... enforced by configuration validation rather than discovered visually."
    [[nodiscard]] Status create(Allocator& allocator, rhi::Device& device,
                                const ManyLightsPassDescription& desc,
                                const ManyLightsSettings& settings) noexcept;
    void destroy() noexcept;
    [[nodiscard]] const ManyLightsSettings& settings() const noexcept { return settings_; }
    /// Change the settings between frames; refused as `create` refuses.
    [[nodiscard]] Status set_settings(const ManyLightsSettings& settings) noexcept;

    /// Import the two targets into this frame's graph, for `AssemblyView`.
    [[nodiscard]] ManyLightsTargets import_targets(RenderGraph& graph) noexcept;
    /// The (slot, view) pairs the frame's set 0 needs for the two targets.
    [[nodiscard]] pipeline::MaterialTextureSlot irradiance_slot(
        rhi::BindlessIndex index) const noexcept;
    [[nodiscard]] pipeline::MaterialTextureSlot reservoir_slot(
        rhi::BindlessIndex index) const noexcept;

    /// This frame's lights and view: after the upload — whose light buffer and view block it
    /// reads — and before executing. The stage is declared without them; a frame recorded with no
    /// `set_frame` dispatches nothing.
    [[nodiscard]] Status set_frame(const ManyLightsFrame& frame) noexcept;
    /// Drop every history: the next frame starts its reservoirs and its accumulation afresh.
    void reset_history() noexcept { history_valid_ = false; }

    [[nodiscard]] FrameStageDeclaration stage() noexcept;
    [[nodiscard]] PassId declare(RenderGraph& graph, const ScreenSpaceStageInputs& inputs) noexcept;

    [[nodiscard]] const ManyLightsReport& report() const noexcept { return report_; }
    /// The stage's device time in the last executed frame, in milliseconds; negative without
    /// timestamp queries. Call after its fence.
    [[nodiscard]] f64 last_device_ms() const noexcept;
    /// Only with `readback`; after the frame's fence.
    [[nodiscard]] Status read_back(ManyLightsReadback& out) const noexcept;

    /// Told the frame ran: the history it wrote is next frame's.
    void executed() noexcept;

    // Public because `RecordFn` is a plain function pointer.
    void record_step(const PassContext& context, u32 step) noexcept;

private:
    enum Persistent : u32 {
        kFinalReservoir = 0,
        kSurface,
        kAccumulated,
        kMoments,
        kPersistentCount,
    };
    struct Texture {
        rhi::TextureHandle texture;
        rhi::TextureViewHandle view;
        rhi::ImageUse use = rhi::ImageUse::Undefined;
    };

    [[nodiscard]] Status create_pipelines() noexcept;
    [[nodiscard]] Status create_textures() noexcept;
    [[nodiscard]] Status create_texture(const char* name, rhi::Format format, bool sampled,
                                        Texture& out) noexcept;
    [[nodiscard]] Status create_buffers() noexcept;
    void dispatch(const PassContext& context, const detail::ComputePipeline& pipeline,
                  u32 filter_pass, rhi::TextureViewHandle filter_in,
                  rhi::TextureViewHandle filter_out) noexcept;
    static PassId declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                void* user) noexcept;
    static void executed_stage(void* user) noexcept;

    rhi::Device* device_ = nullptr;
    Allocator* allocator_ = nullptr;
    ManyLightsPassDescription desc_{};
    ManyLightsSettings settings_{};
    ManyLightConstants constants_{};
    Array<AliasEntry> candidates_{system_allocator(MemoryDomain::Renderer)};
    rhi::BufferHandle light_buffer_;
    ManyLightsReport report_{};
    bool framed_ = false;
    bool history_valid_ = false;
    u32 parity_ = 0;
    u32 filter_passes_ = 0;
    bool declared_ = false;

    detail::ComputePipeline candidates_pipeline_;
    detail::ComputePipeline spatial_pipeline_;
    detail::ComputePipeline accumulate_pipeline_;
    detail::ComputePipeline variance_pipeline_;
    detail::ComputePipeline filter_pipeline_;
    detail::ComputePipeline filter_last_pipeline_;

    Texture persistent_[kPersistentCount][2];
    Texture irradiance_;
    Texture resolved_;
    rhi::BufferHandle words_[rhi::kMaxFramesInFlight];
    rhi::BufferHandle alias_[rhi::kMaxFramesInFlight];
    rhi::QueryPoolHandle timer_;
    bool timed_ = false;

    // Readback.
    rhi::BufferHandle readback_noisy_;
    rhi::BufferHandle readback_guidance_;
    rhi::BufferHandle readback_surface_;
    rhi::BufferHandle readback_irradiance_;
    rhi::BufferHandle readback_resolved_;

    // This frame's resources, from `declare`.
    ResourceId depth_ = kInvalidResource;
    ResourceId normals_ = kInvalidResource;
    ResourceId lights_ = kInvalidResource;
    ResourceId persistent_ids_[kPersistentCount][2] = {};
    ResourceId irradiance_id_ = kInvalidResource;
    ResourceId resolved_id_ = kInvalidResource;
    ResourceId temporal_reservoir_ = kInvalidResource;
    ResourceId guidance_ = kInvalidResource;
    ResourceId noisy_ = kInvalidResource;
    ResourceId variance_ = kInvalidResource;
    ResourceId ping_ = kInvalidResource;
    ResourceId pong_ = kInvalidResource;
    ResourceId readback_ids_[5] = {};
};

}  // namespace cy::rendering::lights_gpu
