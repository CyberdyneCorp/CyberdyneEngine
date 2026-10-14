// SPDX-License-Identifier: MIT
#pragma once
// Illumination diagnostics: causal answers about a point or a region, and the per-frame profile.
// `rendering-global-illumination` — "Illumination diagnostics".
//
// ================================================================================================
// A QUERY ANSWERS "WHY", NOT "WHAT"
// ================================================================================================
//
// `IlluminationSystem::explain(position, normal, surface)` resolves the point exactly as
// `indirect_diffuse` does — the same samples, built by the same function — and reports, beside the
// answer: every source that contributed with its confidence and its share of the weight; every
// radiance-cache probe that contributed with the three terms of its weight, its age and its error,
// and whether the field says the probe cannot see the point (a LEAK SUSPECT: the visibility term
// let it through and geometry is in the way); the world tier a ray from there takes; the region's
// convergence; and one `DarknessCause`. "Why is this area dark" then has four answers that are
// checkable, not adjectives — `Occluded` (it is dark and it should be), `Unconverged`,
// `MissingProbes` and `LowConfidence` — plus `Invalidated` for probes waiting on a change.
//
// ================================================================================================
// THE PROFILE MEASURES THIS SYSTEM'S OWN STAGES, AND ATTRIBUTES CHANGE TO CONTENT
// ================================================================================================
//
// `IlluminationSystem::profile()` is the frame the last `update()` began: the GI allocation and
// what the budget measured, the measured wall time of each stage the specification names (screen
// trace, world trace, probe update, surface cache, denoise, reflection resolve — EXCLUSIVE, so they
// sum to the system's own time), probe counts and updates, rays per tier, the screen-trace hit rate
// and both caches' hit rates, and the invalidations of the frame attributed to the light, object or
// cell that caused them, costliest first. The budget still cannot see a frame time (budget.h): the
// profile is what this system spent, which is the number the budget is handed anyway.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/rendering/gi/gi_volume.h>
#include <cy/rendering/gi/resolve.h>
#include <cy/rendering/gi/scene.h>

namespace cy::rendering::gi {

/// Why a point is as dark as it is.
enum class DarknessCause : u8 {
    /// Converged, confident, and bright enough: nothing to explain.
    Lit = 0,
    /// Converged and confident, and dark because little light reaches it. Correct darkness.
    Occluded,
    /// The probes answering it are still moving.
    Unconverged,
    /// No probe reaches it at all.
    MissingProbes,
    /// Probes answer, with too little confidence to be trusted.
    LowConfidence,
    /// The probes that would answer it were invalidated and have not been gathered again.
    Invalidated,
    Count,
};

[[nodiscard]] const char* darkness_cause_name(DarknessCause cause) noexcept;

struct ExplainSettings {
    /// Below this luminance a converged, confident answer is `Occluded` rather than `Lit`.
    f32 dark_luminance = 0.02F;
    /// The region convergence (resolve.h) below which a point is `Unconverged`.
    f32 converged_threshold = 0.9F;
    /// A contributing probe whose last update moved it by more than this is still converging.
    f32 probe_error_threshold = 0.25F;
    /// Below this resolved confidence a point is `LowConfidence`.
    f32 confidence_floor = 0.4F;
    /// The frame the query is asked in, for the probes' ages.
    u64 frame = 0;
};

/// One source's part in a resolve.
struct SourceReport {
    RadianceSource source = RadianceSource::None;
    Vec3 radiance{0.0F, 0.0F, 0.0F};
    f32 confidence = 0.0F;
    /// Its share of the resolve's confidence weight, in [0, 1]. Zero when excluded.
    f32 weight_share = 0.0F;
    /// Excluded by the double-counting rule.
    bool excluded = false;
};

/// One probe's part in a resolve.
struct ProbeReport {
    u32 probe = 0;
    Vec3 position{0.0F, 0.0F, 0.0F};
    u32 level = 0;
    f32 weight = 0.0F;
    f32 falloff = 0.0F;
    f32 facing = 0.0F;
    f32 visibility = 0.0F;
    u64 age_frames = 0;
    f32 error = 0.0F;
    /// The field blocks the straight line from the probe to the point, and the probe contributes
    /// anyway: the visibility term missed a wall. What the probe-visibility view paints red.
    bool leak_suspect = false;
};

inline constexpr u32 kMaxReportedSources = 4;
inline constexpr u32 kMaxReportedProbes = 32;

/// The answer to "what lit this point, and why is it the colour it is".
struct IlluminationQuery {
    /// Exactly `indirect_diffuse`'s answer for the same arguments.
    ResolveResult diffuse{};
    SourceReport sources[kMaxReportedSources] = {};
    u32 source_count = 0;
    /// The contributing probes, by handle, up to `kMaxReportedProbes`.
    ProbeReport probes[kMaxReportedProbes] = {};
    u32 probe_count = 0;
    /// Every probe that contributed, which may exceed `probe_count`.
    u32 probes_contributing = 0;
    /// Live probes within reach of the point, and how many of them are waiting to be gathered.
    u32 probes_in_reach = 0;
    u32 invalid_probes_in_reach = 0;
    u32 leak_suspects = 0;
    /// The weight of the leak suspects over the whole probe weight.
    f32 leak_weight = 0.0F;
    /// The world tier a ray from here takes, and the GI volumes' preference that chose it.
    RadianceSource world_tier = RadianceSource::None;
    GiTierPreference tier_preference = GiTierPreference::Automatic;
    /// The field's sky visibility about the normal, and its distance to the nearest surface.
    f32 sky_visibility = 0.0F;
    f32 field_distance = 0.0F;
    /// The region's convergence, in [0, 1]; zero where nothing was ever observed.
    f32 convergence = 0.0F;
    /// The GI scene covers the point.
    bool covered = false;
    DarknessCause cause = DarknessCause::Lit;
};

/// The same question asked of a box.
struct IlluminationRegionReport {
    u32 probes = 0;
    u32 valid_probes = 0;
    u32 invalid_probes = 0;
    u64 oldest_age_frames = 0;
    f32 mean_age_frames = 0.0F;
    f32 mean_error = 0.0F;
    f32 max_error = 0.0F;
    /// The least converged of the convergence regions the box overlaps.
    f32 convergence = 0.0F;
    /// Unserviced invalidations overlapping the box, and the serviced ones still in the history.
    u32 pending_invalidations = 0;
    u32 recent_invalidations = 0;
    u32 surface_pages = 0;
    u32 valid_surface_pages = 0;
    DarknessCause cause = DarknessCause::Lit;
};

/// The stages the profile measures. The specification's list, in its order.
enum class GiStage : u8 {
    ScreenTrace = 0,
    WorldTrace,
    ProbeUpdate,
    SurfaceCache,
    Denoise,
    ReflectionResolve,
    Count,
};

inline constexpr u32 kGiStageCount = static_cast<u32>(GiStage::Count);

[[nodiscard]] const char* gi_stage_name(GiStage stage) noexcept;

/// One change's cost: what servicing the invalidations it filed touched.
struct GiCostAttribution {
    u64 source_id = 0;
    InvalidationCause cause = InvalidationCause::GeometryMoved;
    u32 records = 0;
    u32 probes = 0;
    u32 surface_pages = 0;
    u32 field_bricks = 0;
    /// Probes plus pages plus bricks: the ranking key.
    u32 cost = 0;
};

inline constexpr u32 kMaxCostAttributions = 8;

/// One frame of the GI, as a profiler view shows it.
struct GiProfile {
    u64 frame = 0;
    f32 allocation_ms = 0.0F;
    f32 measured_ms = 0.0F;
    /// Wall time per stage, exclusive.
    f64 stage_ms[kGiStageCount] = {};
    u32 probes = 0;
    u32 valid_probes = 0;
    u32 probes_updated = 0;
    u32 probe_queue_depth = 0;
    u32 surface_pages = 0;
    u32 surface_pages_updated = 0;
    /// Rays this frame, by the tier that traced them: the tiered tracer's and the probe gather's.
    u32 screen_rays = 0;
    u32 software_rays = 0;
    u32 hardware_rays = 0;
    /// Screen rays that hit something, over screen rays. Zero when none was traced.
    f32 screen_hit_rate = 0.0F;
    /// Lookups that found a page or a probe, over lookups. One when none was made.
    f32 surface_cache_hit_rate = 1.0F;
    f32 radiance_cache_hit_rate = 1.0F;
    /// This frame's invalidations by what caused them, costliest first.
    GiCostAttribution attribution[kMaxCostAttributions] = {};
    u32 attribution_count = 0;
};

}  // namespace cy::rendering::gi
