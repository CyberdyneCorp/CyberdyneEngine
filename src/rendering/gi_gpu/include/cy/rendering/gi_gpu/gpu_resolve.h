// SPDX-License-Identifier: MIT
#pragma once
// The per-pixel resolve and its denoiser on the device, and the two textures the forward frame
// samples. Issue #35, stage 4.
//
// ================================================================================================
// WHAT IT REPLACES, AND WHAT IT IS HELD TO
// ================================================================================================
//
// `gi::ScreenResolve` (screen_resolve.h) and `denoise::Denoiser`, both on the host, are the oracle.
// A frame of this class is, pass for pass:
//
//   unpack      the `DepthNormal` prepass as surfaces: world position, view depth, normal and
//               roughness a pixel (the frame's path; a test uploads `gi::ScreenSurfaces` instead);
//   reproject   `gi::screen_history`: each pixel's history pixel, through `temporal-rendering`;
//   resolve     the final gather the settings' `gi::GiGather` names, through the world tier
//               `GpuRadianceCache::world_tier()` selects (ray queries where the device has them):
//                 screen probes (the default) — `gi::ScreenProbeGather`'s passes: PLACE the
//                 uniform and adaptive probes, TRACE each probe's rays and blend its reprojected
//                 history, FILTER each probe with its neighbours on its own surface (the
//                 probe-space denoiser, `gi::ScreenProbeDenoiser::ProbeSpace`), INTEGRATE every
//                 cell from the probes about it;
//                 per pixel — `gi::resolve_screen_pixel` for every cell: the radiance cache, one
//                 final-gather ray and the far-field sky, combined by confidence under
//                 `exclusion_for()`'s mask; and the rough specular;
//   accumulate  `Denoiser::accumulate` over that history, per signal;
//   variance    `Denoiser::estimate_variance`;
//   filter      `Denoiser::filter`'s a-trous cascade, one dispatch a pass;
//   publish     the two signals into the indirect diffuse and indirect specular textures.
//
// With no cascade to run — the probe-space denoiser's configuration (`gi::screen_signal_config`) —
// the accumulation is the denoised signal and nothing reads a variance: one pass,
// `cyGiDenoiseTemporal`, accumulates both signals of a pixel and, on the full grid, publishes
// them, over a history layout of its own.
//
// `render.gi_gpu_resolve` runs each against its host function over one view and compares.
//
// ================================================================================================
// IN THE FRAME
// ================================================================================================
//
// `stage()` is the producer of the forward frame's `FramePassKind::ScreenSpaceGi` stage: handed
// the prepass depth and normals and the imported diffuse target, it declares the seven stages
// above, the barriers between them the graph's. The forward pass samples the two targets through
// two slots of its texture table — `write_gi_control` fills the frame's words — where the
// irradiance volume's ambient sits, under the double-counting rule. Off, the stage is not declared,
// no word is written, and the frame is the frame it was.
//
// ================================================================================================
// THE HISTORY, AND FRAMES IN FLIGHT
// ================================================================================================
//
// Two surface slots and two history slots alternate by frame, so a frame reads last frame's while
// it writes its own, with no copy. The constant words are a ring of `rhi::kMaxFramesInFlight`
// buffers, and every descriptor set is allocated per frame, so a frame the host prepares while the
// device still runs the last one writes nothing the device is reading.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/types.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/rendering/denoise/denoiser.h>
#include <cy/rendering/forward/frame.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/screen_resolve.h>
#include <cy/rendering/gi_gpu/gpu_radiance_cache.h>
#include <cy/rendering/graph/graph.h>

namespace cy::rendering::gi_gpu {

class GpuGiDebugView;

struct GpuScreenResolveDescription {
    /// The view's extent. The resolve is full resolution; `ray_density` is how a budget spends
    /// less, and the denoiser's history carries the pixels that did not trace.
    u32 width = 0;
    u32 height = 0;
    /// Host copies of the noisy, reprojected, denoised and surface buffers after each frame, for a
    /// test. A shipped frame reads none of them.
    bool readback = false;
    /// The finest screen-probe spacing the resolve sizes its probe buffers for, in frame pixels. A
    /// setting below it gathers at it.
    u32 min_probe_spacing_pixels = 8;
};

/// One frame's view, as the resolve needs it.
struct GpuResolveView {
    /// The camera's world position.
    Vec3 camera{0.0F, 0.0F, 0.0F};
    /// World to clip, UNJITTERED: what `screen_history` reprojects with.
    Mat4 world_to_clip = Mat4::identity();
    /// For unpacking the prepass: the projection's P00, P11, (2,2) and (3,2) terms — the four
    /// `occlusion::GtaoView` carries — and the view's rotation, camera-relative to view space.
    f32 projection[4] = {1.0F, 1.0F, 0.0F, 0.1F};
    Mat4 relative_to_view = Mat4::identity();
    /// The sub-pixel jitter the prepass was rasterised with, in pixels, in the temporal
    /// framework's convention (+y up) — `occlusion::GtaoView::jitter`'s, undone as GTAO undoes it.
    Vec2 jitter{0.0F, 0.0F};
};

/// GPU time of one frame of the resolve, per stage, from timestamps around the dispatches.
struct GpuResolveTimes {
    f64 unpack_ms = 0.0;
    f64 reproject_ms = 0.0;
    /// The final gather, whichever runs: the probes' three passes, or the per-pixel resolve.
    f64 resolve_ms = 0.0;
    /// The screen probes' passes inside `resolve_ms`; zero for the per-pixel gather, whose one pass
    /// is `integrate_ms`.
    f64 probe_place_ms = 0.0;
    f64 probe_trace_ms = 0.0;
    /// The probe-space filter (`ScreenProbeDenoiser::ProbeSpace`); zero where it does not run.
    f64 probe_filter_ms = 0.0;
    f64 integrate_ms = 0.0;
    f64 accumulate_ms = 0.0;
    f64 variance_ms = 0.0;
    f64 filter_ms = 0.0;
    f64 publish_ms = 0.0;
    f64 total_ms = 0.0;
    bool valid = false;
};

/// `FrameViewData::gi_control`'s first word's bits: where the dynamic GI is admitted, by what the
/// surface carries. `cy/frame.slang`'s `kCyGiAdmit*` are the same numbers.
inline constexpr u32 kGiAdmitPlain = 1U;
inline constexpr u32 kGiAdmitLightmapped = 2U;
inline constexpr u32 kGiAdmitWithVolume = 4U;

/// The admission bits for a GI mode: `exclusion_for()` asked, for each kind of surface, whether it
/// excludes the dynamic diffuse sources. The one place the frame's composite consults the rule.
[[nodiscard]] u32 gi_admission(gi::GiMode mode) noexcept;

class GpuScreenResolve {
public:
    GpuScreenResolve() noexcept = default;
    ~GpuScreenResolve();

    GpuScreenResolve(const GpuScreenResolve&) = delete;
    GpuScreenResolve& operator=(const GpuScreenResolve&) = delete;
    GpuScreenResolve(GpuScreenResolve&&) = delete;
    GpuScreenResolve& operator=(GpuScreenResolve&&) = delete;

    /// Compute and a shader format this module ships.
    [[nodiscard]] static bool supported(const rhi::Device& device) noexcept;

    /// Create over `cache`, which must be created; the resolve reads its probe set, its scene and,
    /// where it has them, its hardware structures.
    [[nodiscard]] Status create(Allocator& allocator, rhi::Device& device, GpuRadianceCache& cache,
                                const GpuScreenResolveDescription& desc) noexcept;
    void destroy() noexcept;

    // --- What the resolve does
    // ---------------------------------------------------------------------

    void set_settings(const gi::ScreenResolveSettings& settings) noexcept;
    [[nodiscard]] const gi::ScreenResolveSettings& settings() const noexcept { return settings_; }
    /// The two signals' configurations and the quality position, read off the host denoiser —
    /// the same table `Denoiser::denoise` filters with.
    void set_denoiser(const denoise::Denoiser& denoiser) noexcept;
    /// The field's sky-visibility reach: `base_extent * 2^(levels - 1)`.
    void set_field(const gi::DistanceField& field) noexcept;
    /// The sky a missed ray and the far field are answered by.
    void set_sky(const gi::SkyTerm& sky) noexcept;
    /// The caches' clock: a probe's and a card's staleness is this frame minus their last update.
    void set_cache_frame(u64 frame) noexcept;
    /// Bring every device copy the resolve reads up to date without gathering anything: the field
    /// the cache was bound to, a host-shaded surface cache's cards and radiance when it was bound
    /// to one, and the probe set. A frame whose radiance cache selected no probe still resolves
    /// against this frame's scene.
    [[nodiscard]] Status sync(const gi::RadianceCache& cache) noexcept;
    /// Forget the history: a cut, a teleport, a resize. The next frame accumulates from nothing.
    void reset_history() noexcept {
        history_valid_ = false;
        probe_history_valid_ = false;
    }

    // --- One frame
    // ---------------------------------------------------------------------------------

    /// Start frame `frame` at `view`: the surface and history slots turn over, and last frame's
    /// view becomes the one the reprojection reads.
    [[nodiscard]] Status begin_frame(const GpuResolveView& view, u64 frame) noexcept;
    /// The prepass's jitter for this frame, which a frame knows once it is assembled — after
    /// `begin_frame`. Pixels, the temporal framework's convention (+y up).
    void set_jitter(Vec2 jitter) noexcept;
    /// The regions this frame invalidated — `gi::ScreenHistoryInputs::invalidated` — after
    /// `begin_frame`, which clears them. At most `gi::kScreenHistoryRegions` are kept.
    void set_invalidated(Span<const Aabb> regions) noexcept;
    /// The resolve grid's divisor (`gi::ScreenGrid`): one resolves every pixel, two a quarter of
    /// them, upsampled back by the publish. A new divisor starts the history over.
    [[nodiscard]] Status set_divisor(u32 divisor) noexcept;
    [[nodiscard]] u32 divisor() const noexcept { return divisor_; }
    [[nodiscard]] u32 grid_width() const noexcept { return grid_width_; }
    [[nodiscard]] u32 grid_height() const noexcept { return grid_height_; }
    /// A test's FRAME surfaces in place of the prepass: their grid written into this frame's slot,
    /// and every pixel's normal and depth into the guidance the publish upsamples against.
    [[nodiscard]] Status set_surfaces(const gi::ScreenSurfaces& surfaces) noexcept;
    /// This frame's surfaces are last frame's: the upload `set_surfaces` wrote is copied into this
    /// frame's slot again without being written again. For a test whose view does not move.
    [[nodiscard]] Status keep_surfaces() noexcept;

    /// Import the two targets into this frame's graph. The diffuse one is the frame's
    /// `FrameDescription::screen_space_gi_target`, the specular its `..._specular_target`.
    [[nodiscard]] ResourceId import_diffuse(RenderGraph& graph) noexcept;
    [[nodiscard]] ResourceId import_specular(RenderGraph& graph) noexcept;

    /// The frame's hook: the `ScreenSpaceGi` stage as this class's passes, from the prepass.
    [[nodiscard]] FrameStageDeclaration stage() noexcept;
    /// Declare the frame from the prepass `inputs` carries. Returns the first pass, or
    /// `kInvalidPass` when the inputs are not the ones this frame was prepared for.
    [[nodiscard]] PassId declare(RenderGraph& graph, const ScreenSpaceStageInputs& inputs) noexcept;
    /// Declare the frame from the surfaces `set_surfaces` wrote: every stage but the unpack.
    [[nodiscard]] Status declare_from_surfaces(RenderGraph& graph) noexcept;

    [[nodiscard]] rhi::TextureViewHandle diffuse_view() const noexcept { return diffuse_view_; }
    [[nodiscard]] rhi::TextureViewHandle specular_view() const noexcept { return specular_view_; }
    [[nodiscard]] static constexpr rhi::Format target_format() noexcept {
        return rhi::Format::Rgba16Sfloat;
    }

    // --- After the frame's fence (`readback`) ---------------------------------------------------

    /// The grid's buffers, one entry a grid cell: the resolve's noisy signals, the denoised ones,
    /// the history and the surfaces.
    [[nodiscard]] Status read_back_noisy(Span<Vec3> diffuse, Span<Vec3> specular) const noexcept;
    /// Created with `readback`: whether the frames begun from now on copy their buffers for the
    /// host — every frame unless turned off. The copies, and the published signal the temporal
    /// pass writes for them, cost what a frame nobody reads back should not pay, and their
    /// writes land inside the stages the timestamps time: a capture that reads only its last
    /// frame turns them off for the frames before it.
    void set_readback_frames(bool copy) noexcept { readback_frames_ = copy; }
    [[nodiscard]] Status read_back_denoised(Span<Vec3> diffuse, Span<Vec3> specular) const noexcept;
    [[nodiscard]] Status read_back_history(Span<i32> source, Span<f32> confidence) const noexcept;
    /// This frame's surfaces: world position, view depth, normal, roughness.
    [[nodiscard]] Status read_back_surfaces(Span<Vec3> position, Span<f32> depth, Span<Vec3> normal,
                                            Span<f32> roughness) const noexcept;
    /// The published signals, one entry a FRAME pixel: what the forward pass samples.
    [[nodiscard]] Status read_back_published(Span<Vec3> diffuse,
                                             Span<Vec3> specular) const noexcept;
    /// This frame's screen probes: `probe_layout().records()` records (the uniform probes, then the
    /// adaptive slots in the order the device took them) and the slot of each half-tile cell.
    /// Returns how many adaptive slots were taken.
    [[nodiscard]] Expected<u32, Error> read_back_probes(Span<gi::ScreenProbe> records,
                                                        Span<u32> slots) const noexcept;
    /// The probe layout of the resolve grid at the settings' spacing.
    [[nodiscard]] const gi::ScreenProbeLayout& probe_layout() const noexcept {
        return probe_layout_;
    }

    // --- Timing
    // ------------------------------------------------------------------------------------

    void set_timing(bool enabled) noexcept { timing_ = enabled; }
    [[nodiscard]] GpuResolveTimes last_times() const noexcept;
    /// Which world tier the last declared resolve traced through.
    [[nodiscard]] gi::RadianceSource last_tier() const noexcept { return last_tier_; }

    [[nodiscard]] const GpuScreenResolveDescription& description() const noexcept { return desc_; }

private:
    friend class GpuGiDebugView;

    /// The ray query pipelines last: a device without ray queries creates the ones before them.
    enum Pipeline : u32 {
        kUnpack = 0,
        kReproject,
        kAccumulate,
        kVariance,
        kFilter,
        kPublish,
        kResolve,
        kProbePlace,
        kProbeTrace,
        kProbeIntegrate,
        kProbeFilter,
        kTemporal,
        /// The probe trace in 32-lane workgroups, for probes of at most 32 rays.
        kProbeTraceNarrow,
        /// The adaptive probes' slots, in cell order: one workgroup after the placement.
        kProbeRank,
        kResolveHardware,
        kProbeTraceHardware,
        kProbeIntegrateHardware,
        kProbeTraceHardwareNarrow,
        kPipelineCount,
    };
    enum Layout : u32 {
        kSurfaceLayout = 0,
        kResolveLayout,
        kDenoiseLayout,
        kPublishLayout,
        kTemporalLayout,
        kLayoutCount,
    };
    enum Buffer : u32 {
        kSurfaces = 0,
        kNoisy,
        kSources,
        kHistory,
        kVariances,
        kFilterA,
        kFilterB,
        kNoisyHost,
        kSourcesHost,
        kFinalHost,
        kSurfacesHost,
        /// One slot of surfaces as `set_surfaces` writes them, copied into this frame's slot by a
        /// declared transfer. Created by the first `set_surfaces`: a frame never needs it.
        kSurfacesUpload,
        /// [normal, view depth] a FRAME pixel: what the publish upsamples the grid against.
        kGuidance,
        kGuidanceUpload,
        /// The upsampled signals a frame pixel, kept when `readback` asks.
        kPublished,
        kPublishedHost,
        /// The screen probes: two frame slots of records, and the adaptive counters and tables.
        kProbes,
        kProbeSlots,
        /// Zeros for the counters, copied in by the first frame.
        kProbeSlotsUpload,
        kProbesHost,
        kProbeSlotsHost,
        kBufferCount,
    };
    /// Timestamps: one before the first stage and one after each of the ten — unpack, reproject,
    /// place, trace, probe filter, integrate, accumulate, variance, filter, publish. A stage the
    /// frame does not run is an empty interval: the per-pixel gather's three probe stages, the
    /// probe filter under the shared denoiser, the variance and cascade where there is none.
    static constexpr u32 kStamps = 11;

    struct Step {
        GpuScreenResolve* self = nullptr;
        Pipeline pipeline = kReproject;
        /// The stamp written after the dispatch, or ~0 for none; a stage's last pass writes it.
        u32 stamp = ~0U;
        /// Whether this pass also writes stamp 0 before its dispatch: the frame's first.
        bool first = false;
        u32 count = 0;
        u32 pass = 0;
        u32 direction = 0;
        ResourceId depth = kInvalidResource;
        ResourceId normals = kInvalidResource;
        ResourceId diffuse = kInvalidResource;
        ResourceId specular = kInvalidResource;
        /// Stamps written before the dispatch: the probe stages the per-pixel gather skips.
        u32 empty_before = 0;
        /// Stamps written after its own: the stages a frame whose last pass this is skipped.
        u32 empty_after = 0;
    };
    struct Copy {
        GpuScreenResolve* self = nullptr;
        Buffer source = kNoisy;
        Buffer destination = kNoisyHost;
        u64 offset = 0;
        u64 bytes = 0;
        u64 target_offset = 0;
    };

    [[nodiscard]] Status create_layouts() noexcept;
    [[nodiscard]] Status create_pipelines() noexcept;
    /// One pipeline, unless it exists.
    [[nodiscard]] Status create_pipeline(u32 which) noexcept;
    /// The probe trace's variant for the settings' rays, through either tier.
    [[nodiscard]] Pipeline trace_pipeline(bool hardware) const noexcept;
    /// Create the trace variants the settings ask for, if they do not exist yet.
    [[nodiscard]] Status ensure_trace_pipelines() noexcept;
    [[nodiscard]] Status create_resources() noexcept;
    void write_constants() noexcept;
    [[nodiscard]] rhi::BufferHandle constants() const noexcept;
    [[nodiscard]] ResourceId import(RenderGraph& graph, Buffer buffer) noexcept;
    [[nodiscard]] ResourceId import_constants(RenderGraph& graph) noexcept;
    [[nodiscard]] u32 filter_passes() const noexcept;
    void declare_denoiser(RenderGraph& graph, ResourceId diffuse, ResourceId specular) noexcept;
    void declare_publish(RenderGraph& graph, ResourceId diffuse, ResourceId specular) noexcept;
    void declare_readbacks(RenderGraph& graph) noexcept;
    void declare_copy(RenderGraph& graph, Copy& copy, const char* name) noexcept;
    [[nodiscard]] PassId declare_resolve(RenderGraph& graph, bool first) noexcept;
    void declare_probes(RenderGraph& graph, bool hardware) noexcept;
    void update_probe_words() noexcept;
    /// The probe-space filter's reach in tiles this frame, or zero where it does not run.
    [[nodiscard]] u32 probe_filter_tiles() const noexcept;
    /// The next step in `steps_`, its stamp's empty predecessors counted.
    [[nodiscard]] Step& add_step(Pipeline pipeline, u32 stamp, u32 count) noexcept;
    void update_filter_passes() noexcept;

    static PassId declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                void* user) noexcept;
    static void record_step(const PassContext& context, void* user) noexcept;
    static void record_copy(const PassContext& context, void* user) noexcept;
    [[nodiscard]] Expected<rhi::DescriptorSetHandle, Error> transient_set(
        const PassContext& context, const Step& step) noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    GpuRadianceCache* cache_ = nullptr;
    GpuScreenResolveDescription desc_{};
    gi::ScreenResolveSettings settings_{};
    /// `set_readback_frames`: the frames begun copy for the host, when created with `readback`.
    bool readback_frames_ = true;
    /// What creating the trace pipelines a `set_settings` asked for answered; `begin_frame` returns
    /// it.
    Status pipeline_status_ = ok();
    [[nodiscard]] bool copying() const noexcept { return desc_.readback && readback_frames_; }

    rhi::ShaderModuleHandle shaders_[kPipelineCount];
    rhi::ComputePipelineHandle pipelines_[kPipelineCount];
    rhi::DescriptorSetLayoutHandle layouts_[kLayoutCount];
    rhi::PipelineLayoutHandle surface_pipeline_layout_;
    rhi::PipelineLayoutHandle resolve_pipeline_layout_;
    rhi::PipelineLayoutHandle resolve_hardware_pipeline_layout_;
    rhi::PipelineLayoutHandle denoise_pipeline_layout_;
    rhi::PipelineLayoutHandle publish_pipeline_layout_;
    rhi::PipelineLayoutHandle temporal_pipeline_layout_;
    bool hardware_pipeline_ = false;

    rhi::BufferHandle ring_[rhi::kMaxFramesInFlight];
    rhi::BufferHandle buffers_[kBufferCount];
    u64 sizes_[kBufferCount] = {};
    rhi::TextureHandle diffuse_;
    rhi::TextureViewHandle diffuse_view_;
    rhi::TextureHandle specular_;
    rhi::TextureViewHandle specular_view_;
    rhi::QueryPoolHandle timer_;
    bool timing_ = false;
    bool timed_ = false;

    /// The constant words: 64 uint4, the layout gi_resolve_common.slang states.
    u32 words_[256] = {};
    u32 ring_slot_ = 0;
    u32 slot_ = 0;
    bool history_valid_ = false;
    bool view_valid_ = false;
    bool began_ = false;
    Mat4 previous_world_to_clip_ = Mat4::identity();
    gi::RadianceSource last_tier_ = gi::RadianceSource::None;

    ResourceId imported_diffuse_ = kInvalidResource;
    ResourceId imported_specular_ = kInvalidResource;
    Step steps_[20]{};
    u32 step_count_ = 0;
    Copy copies_[7]{};
    Copy upload_{};
    Copy counters_upload_{};
    Copy guidance_upload_{};
    gi::ScreenGrid grid_;
    u32 divisor_ = 1;
    u32 grid_width_ = 0;
    u32 grid_height_ = 0;
    bool surfaces_uploaded_ = false;
    bool final_in_b_ = false;
    gi::ScreenProbeLayout probe_layout_{};
    /// Whether last frame traced the probes this frame's reprojection reads, at this layout.
    bool probe_history_valid_ = false;
    bool counters_cleared_ = false;
    /// Each signal's cascade length and history length as the host denoiser configures them,
    /// before `gi::screen_signal_config` applies the gather's.
    u32 authored_passes_[2] = {0, 0};
    u32 authored_history_[2] = {32, 32};
    f32 authored_rejection_[2] = {0.0F, 0.0F};
    /// The stamp the next declared step's interval starts at.
    u32 next_stamp_ = 0;
};

/// The frame's words for the two targets bound at `diffuse_slot` and `specular_slot` of the set 0
/// texture table, under `mode`: `pipeline::FrameViewData::gi_control`.
void write_gi_control(u32 diffuse_slot, u32 specular_slot, gi::GiMode mode, u32 out[4]) noexcept;

}  // namespace cy::rendering::gi_gpu
