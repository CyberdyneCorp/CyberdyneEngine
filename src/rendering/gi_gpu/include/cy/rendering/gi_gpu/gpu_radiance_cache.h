// SPDX-License-Identifier: MIT
#pragma once
// The radiance cache and the tracing tiers on the device. Issue #35, stage 3.
//
// ================================================================================================
// WHAT IT REPLACES, AND BEHIND WHICH SEAM
// ================================================================================================
//
// `gi::RadianceCache::gather_probe` — the arithmetic of the radiance cache, never its decisions.
// `GpuRadianceCache` is a `gi::ProbeGatherBackend`: install it with
// `IlluminationSystem::set_probe_gather` (or `RadianceCache::set_gather_backend`) and the cache
// keeps placing probes ADAPTIVELY, scrolling its clipmaps, invalidating, SELECTING with the
// progress-guaranteeing scheduler (`RadianceCache::select`) and answering queries exactly as
// before, while the probes it selects are gathered here: rays from the probe through the uploaded
// field (the software tier) or through the acceleration service's structures with ray queries (the
// hardware tier), every hit resolved through the device surface cache — a lookup, never a material.
//
// ================================================================================================
// THE DEVICE COPY, AND WHAT IS WRITTEN WHEN
// ================================================================================================
//
//   probes     position and level, last update and validity, the six axis distances (the
//              visibility term), mirrored when a probe's `revision` moved — creation, retirement,
//              invalidation, a seed — and written in place by the gather for the probes it gathers;
//   payloads   one probe's encoding, the same rule;
//   layout     each clipmap level's window of cells, toroidally addressed, naming the probe a cell
//              holds: what the device query walks instead of the host's spatial index;
//   directions the frame's ray table — `gi::probe_ray_direction` for the frame's rotation, once
//              per update rather than once per probe, so both sides trace bit-identical rays.
//
// ================================================================================================
// THE TIERS, AND THE ONE PREDICATE THAT CHOOSES BETWEEN THEM
// ================================================================================================
//
// `world_tier()` is hardware when, and only when, the hardware pipelines exist (the device reports
// `Capability::RayTracing`), the service is ACTIVE (`rt::AccelerationService::active()`, which a
// profile switches off) and its top level is built on the device. That is
// `gi::HardwareTracer::available()` with a device behind it, so a device without ray queries and a
// profile that disabled them take the one software path, and nothing downstream branches on which.
//
// `GpuTierTracer` (below) is the tiers on their own: a batch of rays through the screen tier
// against a nearest-depth pyramid and last frame's colour, through either world tier, or through
// the escalation `gi::TieredTracer` makes between them.
//
// NOT IN THE FRAME. Nothing here composites anything; stage 4 reads `declare_sample`'s query and
// the tiers from the per-pixel resolve.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/radiance_cache.h>
#include <cy/rendering/gi/scene.h>
#include <cy/rendering/gi/surface_cache.h>
#include <cy/rendering/gi/tracing.h>
#include <cy/rendering/gi_gpu/gpu_scene.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/raytracing/acceleration.h>
#include <cy/rendering/raytracing_gpu/device_structures.h>

namespace cy::rendering::gi_gpu {

/// The most clipmap levels the probe set's constant block addresses.
inline constexpr u32 kMaxProbeLevels = 8;
/// The most rays one probe gathers per update: a probe is gathered by one 64-lane workgroup.
inline constexpr u32 kMaxProbeRays = 64;
/// The most pyramid levels the screen tier's nearest-depth pyramid has: enough for 8192 pixels.
inline constexpr u32 kMaxScreenLevels = 14;

struct GpuRadianceCacheDescription {
    u32 max_probes = 4096;
    /// The most probes one update gathers: the GI budget's `ProbeUpdates` ceiling.
    u32 max_selection = 1024;
    /// At most `kMaxProbeRays`: a probe's rays are one workgroup's lanes.
    u32 max_rays_per_probe = 64;
    /// The most points one `declare_sample` answers.
    u32 max_queries = 4096;
    /// The most rays one tier batch carries, and the largest screen the screen tier reads.
    u32 max_tier_rays = 65536;
    u32 max_screen_pixels = 65536;
};

/// What the last `submit` handed the device.
struct GpuProbeSubmission {
    u64 frame = 0;
    u32 probes = 0;
    u32 rays_per_probe = 0;
    /// `HardwareTrace` or `SoftwareTrace`: the tier the gather traced through.
    gi::RadianceSource tier = gi::RadianceSource::None;
    /// Probe records and payloads written because their revision moved.
    u32 probes_uploaded = 0;
    bool layout_rebuilt = false;
};

/// One point the device query answers.
struct GpuProbeQuery {
    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 normal{0.0F, 1.0F, 0.0F};
};

/// GPU time of the last timed dispatch, in milliseconds, from timestamps around it alone.
struct GpuDispatchTime {
    f64 milliseconds = 0.0;
    bool valid = false;
};

class GpuTierTracer;

class GpuRadianceCache : public gi::ProbeGatherBackend {
public:
    GpuRadianceCache() noexcept = default;
    ~GpuRadianceCache() override;

    GpuRadianceCache(const GpuRadianceCache&) = delete;
    GpuRadianceCache& operator=(const GpuRadianceCache&) = delete;
    GpuRadianceCache(GpuRadianceCache&&) = delete;
    GpuRadianceCache& operator=(GpuRadianceCache&&) = delete;

    /// Create the probe set over `scene`, which must already be created on `device`. The hardware
    /// pipelines are created when the device reports `Capability::RayTracing` and never otherwise,
    /// so a device without ray queries creates exactly the software entry points.
    [[nodiscard]] Status create(Allocator& allocator, rhi::Device& device, GpuGiScene& scene,
                                const GpuRadianceCacheDescription& desc) noexcept;
    void destroy() noexcept;

    /// The field the gather traces, uploaded at each submit (nothing, when it did not scroll).
    void bind_field(const gi::DistanceField& field) noexcept { field_ = &field; }
    /// A HOST-shaded surface cache whose radiance every submit uploads. Null when the device
    /// surface cache (`GpuSurfaceShading`) keeps the card state itself, which is the frame's case.
    void bind_host_surfaces(const gi::SurfaceCache* surfaces) noexcept { surfaces_ = surfaces; }
    /// The hardware tier: the service whose decisions the structures mirror, and the structures.
    /// Either may be null, which is the software tier.
    [[nodiscard]] Status bind_hardware(const rt::AccelerationService* service,
                                       rt_gpu::DeviceStructures* structures) noexcept;

    /// Which world tier a gather or a tier batch traces through now. See the header comment.
    [[nodiscard]] gi::RadianceSource world_tier() const noexcept;
    /// Whether the hardware pipelines exist at all: the device reports ray queries.
    [[nodiscard]] bool hardware_supported() const noexcept { return hardware_pipelines_; }

    // --- gi::ProbeGatherBackend ------------------------------------------------------------------

    [[nodiscard]] u32 submit(const gi::RadianceCache& cache, Span<const u32> selected,
                             const gi::ProbeUpdateContext& context) noexcept override;
    u32 retire(gi::RadianceCache& cache) noexcept override;

    /// Declare the gather and the copy of its results to the host. Call once after each `submit`
    /// that accepted probes; a submission never declared is dropped at `retire`.
    [[nodiscard]] Status declare(RenderGraph& graph) noexcept;

    // --- The query: `RadianceCache::sample` on the device
    // -----------------------------------------

    /// Bring the device copy of `cache`'s probes up to date without gathering anything: what a
    /// query between updates, or a cache whose probes the host gathered, needs first.
    [[nodiscard]] Status upload(const gi::RadianceCache& cache) noexcept;
    /// Forget what was uploaded, so the next `upload` or `submit` writes every probe. A mirror does
    /// it on its own when `RadianceCache::generation()` moves — a configure starts the probe slots
    /// and their revisions over.
    void reset_mirror() noexcept;
    [[nodiscard]] Status set_queries(Span<const GpuProbeQuery> queries) noexcept;
    /// Declare the query over the batch `set_queries` wrote, at `frame` for the staleness.
    [[nodiscard]] Status declare_sample(RenderGraph& graph, u64 frame) noexcept;
    /// After the frame's fence: one sample per query, `RadianceSource::RadianceCache`.
    [[nodiscard]] Status read_back_samples(Span<gi::RadianceSample> out) const noexcept;

    // --- Timing ---------------------------------------------------------------------------------

    /// Bracket the next declared gather, query or tier batch with timestamps.
    void set_timing(bool enabled) noexcept { timing_ = enabled; }
    [[nodiscard]] GpuDispatchTime last_time() const noexcept;

    [[nodiscard]] const GpuProbeSubmission& last_submission() const noexcept { return submission_; }
    [[nodiscard]] const Error& last_error() const noexcept { return error_; }
    [[nodiscard]] const GpuRadianceCacheDescription& description() const noexcept { return desc_; }

private:
    friend class GpuTierTracer;

    // `GiProbeSet` order, then the host copies no shader binds.
    enum Binding : u32 {
        kConstants = 0,
        kProbes,
        kPayloads,
        kSelection,
        kDirections,
        kResults,
        kLayout,
        kQueries,
        kSamples,
        kTierRays,
        kTierResults,
        kScreenDepth,
        kScreenColour,
        kScreenHzb,
        kCounters,
        kBindingCount,
        kResultsHost = kBindingCount,
        kSamplesHost,
        kTierResultsHost,
        kCountersHost,
        kBufferCount,
    };
    enum Pipeline : u32 {
        kGather = 0,
        kSample,
        kBuildHzb,
        kTraceScreen,
        kTraceSoftware,
        kTraceTiered,
        kGatherHardware,
        kTraceHardware,
        kTraceTieredHardware,
        kPipelineCount,
        kFirstHardware = kGatherHardware,
    };
    struct Recording {
        GpuRadianceCache* self = nullptr;
        Pipeline pipeline = kGather;
        GpuGiScene::Dispatch dispatch{};
        bool timed = false;
        /// Workgroups to dispatch; zero for one lane per item, 64 items a group.
        u32 groups = 0;
    };
    struct Copy {
        GpuRadianceCache* self = nullptr;
        Binding source = kResults;
        Binding destination = kResultsHost;
        u64 bytes = 0;
    };

    [[nodiscard]] Status create_layouts() noexcept;
    [[nodiscard]] Status create_pipelines() noexcept;
    [[nodiscard]] Status create_buffers() noexcept;
    [[nodiscard]] Status write_descriptors() noexcept;
    [[nodiscard]] Status write_hardware_descriptors() noexcept;
    [[nodiscard]] void* mapped(Binding binding) const noexcept;
    void write_constants() noexcept;
    void write_sky(const gi::SkyTerm& sky) noexcept;
    [[nodiscard]] Status mirror(const gi::RadianceCache& cache) noexcept;
    void write_layout(const gi::RadianceCache& cache) noexcept;
    [[nodiscard]] ResourceId import(RenderGraph& graph, Binding binding) noexcept;
    /// The reads every dispatch over set 0 and set 1 shares, and set 2's when `hardware`.
    void read_scene(RenderGraph& graph, PassBuilder& pass, bool hardware) noexcept;
    void declare_copy(RenderGraph& graph, Copy& copy, const char* name) noexcept;

    static void record_dispatch(const PassContext& context, void* user) noexcept;
    static void record_copy(const PassContext& context, void* user) noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    GpuGiScene* scene_ = nullptr;
    const gi::DistanceField* field_ = nullptr;
    const gi::SurfaceCache* surfaces_ = nullptr;
    const rt::AccelerationService* service_ = nullptr;
    rt_gpu::DeviceStructures* structures_ = nullptr;
    GpuRadianceCacheDescription desc_{};

    rhi::DescriptorSetLayoutHandle probe_layout_;
    rhi::DescriptorSetLayoutHandle hardware_layout_;
    rhi::PipelineLayoutHandle software_pipeline_layout_;
    rhi::PipelineLayoutHandle hardware_pipeline_layout_;
    rhi::DescriptorSetHandle probe_set_;
    rhi::DescriptorSetHandle hardware_set_;
    rhi::ShaderModuleHandle shaders_[kPipelineCount];
    rhi::ComputePipelineHandle pipelines_[kPipelineCount];
    bool hardware_pipelines_ = false;
    rhi::BufferHandle buffers_[kBufferCount];
    u64 sizes_[kBufferCount] = {};
    rhi::QueryPoolHandle timer_;
    bool timing_ = false;
    bool timed_ = false;

    /// The probe set's constant words: 64 uint4, the layout gi_probe_common.slang states.
    u32 constants_[256] = {};

    /// The revision of each probe slot as last uploaded, and whether the slot has been seen.
    Array<u32> revisions_;
    Array<u8> seen_;
    /// `RadianceCache::generation()` as last mirrored.
    u64 generation_ = ~0ULL;
    /// The clipmap centre the layout was last written for, per level.
    i32 centres_[kMaxProbeLevels][3] = {};
    bool layout_valid_ = false;

    GpuProbeSubmission submission_{};
    Array<u32> handles_;
    Array<u32> submitted_revisions_;
    bool declared_ = false;
    u32 query_count_ = 0;
    Error error_{};

    Recording gather_{};
    Recording sample_{};
    Copy results_copy_{};
    Copy samples_copy_{};
};

// --- The tiers on their own ----------------------------------------------------------------------

/// One ray of a tier batch: `TieredTracer::trace`'s arguments.
struct GpuTierRay {
    Vec3 origin{0.0F, 0.0F, 0.0F};
    Vec3 direction{0.0F, 0.0F, -1.0F};
    f32 max_distance = 0.0F;
};

/// Which tiers a batch runs.
enum class GpuTierMode : u8 {
    /// `ScreenTracer::trace` alone, stepping every step.
    Screen = 0,
    /// The same answer, skipping the runs of steps the nearest-depth pyramid proves empty.
    ScreenAccelerated,
    /// The world tier `GpuRadianceCache::world_tier()` selects, alone.
    World,
    /// `TieredTracer::trace`: the accelerated screen tier, escalating to the world tier.
    Tiered,
};

/// What a batch counted, as `gi::TracingDiagnostics` counts it, plus the screen tier's reads.
struct GpuTierCounters {
    u32 world_rays = 0;
    u32 escalations = 0;
    u32 budget_refusals = 0;
    /// Depth-buffer and pyramid texels the screen tier read, over the whole batch.
    u32 depth_reads = 0;
    u32 screen_rays = 0;
    u32 software_rays = 0;
    u32 hardware_rays = 0;
    u32 sky_answers = 0;
};

/// The tracing tiers on the device, over a `GpuRadianceCache`'s probe set and its scene.
class GpuTierTracer {
public:
    explicit GpuTierTracer(GpuRadianceCache& cache) noexcept : cache_(&cache) {}

    /// The screen tier's buffers, or null for a batch with no screen (a probe gather has none).
    /// Uploads the depth and last frame's colour; the pyramid is rebuilt by the next declare.
    [[nodiscard]] Status set_view(const gi::ScreenView* view) noexcept;
    /// The escalation threshold, the reach, and the sky a miss is answered by.
    void set_config(const gi::TieredTracer::Config& config) noexcept;
    /// What a `Tiered` batch may spend: `TraceBudget` for the whole batch.
    void set_budget(const gi::TraceBudget& budget) noexcept;
    /// The surface cache's clock: a hit's staleness is this frame minus the card's last update.
    void set_cache_frame(u64 frame) noexcept;

    [[nodiscard]] Status set_rays(Span<const GpuTierRay> rays) noexcept;
    [[nodiscard]] Status declare(RenderGraph& graph, GpuTierMode mode) noexcept;
    /// After the frame's fence: one sample per ray, and the batch's counters.
    [[nodiscard]] Status read_back(Span<gi::RadianceSample> out,
                                   GpuTierCounters& counters) const noexcept;

private:
    GpuRadianceCache* cache_ = nullptr;
    u32 ray_count_ = 0;
    u32 hzb_levels_ = 0;
    bool screen_ = false;
    bool hzb_dirty_ = false;
    GpuRadianceCache::Recording hzb_[kMaxScreenLevels]{};
    GpuRadianceCache::Recording trace_{};
    GpuRadianceCache::Copy results_copy_{};
    GpuRadianceCache::Copy counters_copy_{};
};

}  // namespace cy::rendering::gi_gpu
