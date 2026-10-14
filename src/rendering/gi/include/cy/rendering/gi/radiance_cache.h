#pragma once
// The radiance cache: world-space probes, adaptively placed, scheduled under a budget. Task 9.1.
//
// `rendering-global-illumination` — "Radiance cache", "Probe update scheduling".
//
// ================================================================================================
// A PROBE IS QUERIED BY MANY PIXELS, WHICH IS THE WHOLE ECONOMIC ARGUMENT
// ================================================================================================
//
// Solving indirect diffuse per pixel costs pixels times rays. Solving it per probe costs probes
// times rays, and a probe serves hundreds of pixels. Everything else here follows from wanting
// probes where they buy something:
//
//   * **adaptive, not a uniform grid.** Density is high near surfaces, at corners, near lights and
//     inside a marked importance region, and low in open space. A probe inside solid geometry is
//     not allocated at all — it would be queried by nothing and updated forever.
//   * **clipmaps.** Levels of increasing spacing centred on the camera, so a large world is covered
//     at decreasing density, and a camera that moves scrolls them and reuses what is still valid.
//   * **a visibility term.** A probe on the far side of a wall interpolated into a query is the
//     single most recognisable GI artefact there is. Each probe records how far the world is in six
//     directions, and a query further away than that in the probe's direction weights it to zero.
//
// ================================================================================================
// THE SCHEDULER GUARANTEES PROGRESS, WHICH IS NOT THE SAME AS BEING FAIR
// ================================================================================================
//
// Priority combines visibility, distance, invalidation recency, age, estimated error and regional
// importance — and then a probe older than `max_age_frames` jumps ahead of all of it. Without that
// last rule a region that is never visible is never updated, which is not "low priority" but
// "starved", and it is the difference between a cache that lags and a cache that is wrong.
//
// ================================================================================================
// THREE ENCODINGS, AND THE TRADE-OFF IS A TABLE RATHER THAN A PARAGRAPH
// ================================================================================================
//
// `encoding_traits()` gives memory, directionality and evaluation cost for each. They are real
// alternatives, not a switch with one branch implemented: the spherical-Gaussian form uses the six
// cardinal axes at a fixed sharpness, which is what makes it cheap and what it gives up.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/bvh.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/gi_volume.h>
#include <cy/rendering/gi/lighting.h>
#include <cy/rendering/gi/scene.h>

namespace cy::rendering::gi {

/// How a probe stores the radiance arriving at it.
enum class ProbeEncoding : u8 {
    /// Four coefficients per channel. The cheapest to store and to evaluate, and the least
    /// directional: it cannot represent a shadow boundary across the probe's own sphere.
    SphericalHarmonicsL1 = 0,
    /// A 4x4 octahedral irradiance map. Four times the memory and the most directional of the
    /// three, which is what an interior with a bright doorway needs.
    Octahedral,
    /// Six lobes on the cardinal axes at a fixed sharpness. Between the two in every respect, and
    /// its fixed axes are the thing it gives up: it cannot concentrate resolution where the
    /// radiance actually varies.
    SphericalGaussian,
    Count,
};

[[nodiscard]] const char* probe_encoding_name(ProbeEncoding encoding) noexcept;

/// The trade-off, as numbers. `directionality` and `evaluation_cost` are relative to
/// `SphericalHarmonicsL1` at 1.0.
struct EncodingTraits {
    u32 floats_per_probe = 0;
    u32 bytes_per_probe = 0;
    f32 directionality = 1.0F;
    f32 evaluation_cost = 1.0F;
};

[[nodiscard]] EncodingTraits encoding_traits(ProbeEncoding encoding) noexcept;

/// The largest payload any encoding needs, in floats. The octahedral map's 16 texels of RGB.
inline constexpr u32 kMaxProbeFloats = 48;

struct ProbeCacheSettings {
    /// Clipmap levels. Level 0 has `base_spacing_metres` between probes and each level doubles it.
    u32 levels = 3;
    f32 base_spacing_metres = 1.0F;
    /// Probes each side of the camera per axis, per level. The window is twice this.
    u32 half_extent_probes = 4;
    ProbeEncoding encoding = ProbeEncoding::SphericalHarmonicsL1;
    /// How far outside a probe's recorded world distance a query may sit before the probe is
    /// treated as being on the other side of a wall. In metres.
    f32 visibility_bias_metres = 0.25F;
};

struct Probe {
    Vec3 position{0.0F, 0.0F, 0.0F};
    u32 level = 0;
    /// The distance to the world along each of the six cardinal axes, in metres. The visibility
    /// term: a query further away than this in the probe's direction is behind something.
    f32 axis_distance[6] = {};
    u64 last_update_frame = 0;
    f32 error = 1.0F;
    f32 importance = 1.0F;
    f32 camera_distance = 0.0F;
    bool visible = true;
    bool valid = false;
    bool live = false;
    /// Bumped by every change the host makes to the probe — creation, retirement, invalidation and
    /// a seed — and by nothing a gather does. A device mirror compares it against the revision it
    /// last uploaded; a gather backend compares it against the revision it submitted, which is how
    /// a result for a probe invalidated while its gather was in flight is dropped rather than
    /// undoing the invalidation. The same contract as `SurfacePage::revision`.
    u32 revision = 0;
    /// What the GI volumes resolved to at this probe when it was placed (gi_volume.h). Neutral —
    /// one, one, `Automatic` — where no volume reaches, which is every probe of a scene with none.
    f32 update_rate = 1.0F;
    f32 ray_scale = 1.0F;
    GiTierPreference tier = GiTierPreference::Automatic;
};

struct ProbeUpdateReport {
    u32 probes_updated = 0;
    u32 queue_depth = 0;
    u64 oldest_unserviced_age = 0;
    u32 rays = 0;
    f32 mean_error = 0.0F;
};

struct ProbePlacementReport {
    u32 probes_created = 0;
    u32 probes_reused = 0;
    u32 probes_retired = 0;
    u32 rejected_inside_geometry = 0;
    u32 rejected_open_space = 0;
    /// Candidates a GI volume's density placed although the geometry would not have, and ones it
    /// refused although the geometry would have.
    u32 placed_by_volume_density = 0;
    u32 rejected_by_volume_density = 0;
};

struct RadianceCacheDiagnostics {
    u32 probe_count = 0;
    u32 valid_probes = 0;
    u64 bytes = 0;
    ProbePlacementReport last_placement{};
    ProbeUpdateReport last_update{};
    /// Queries answered with no probe at all. A non-zero number here is why an area is dark.
    u32 gather_misses = 0;
    u32 gathers = 0;
};

/// What one probe update needs. `tracer` may be null, in which case probes gather the sky only —
/// which is exactly what `GiMode::None` is and is not an error.
struct ProbeUpdateContext {
    const SceneTracer* tracer = nullptr;
    const RadianceLookup* radiance = nullptr;
    SkyTerm sky{};
    u32 rays_per_probe = 32;
    u32 budget = 64;
    u64 frame = 0;
    u64 max_age_frames = 300;
    f32 max_ray_distance_metres = 40.0F;
    /// Converged mode: every probe is updated, however many there are. Used by golden-image tests,
    /// cinematic capture and the bake's seeding.
    bool converged = false;
    /// The tracer a probe whose GI volume prefers `SoftwareOnly` gathers through. Null means every
    /// probe uses `tracer`.
    const SceneTracer* software_tracer = nullptr;
};

/// What the placement pass needs in order to decide where a probe buys something.
struct ProbePlacementContext {
    const DistanceField* field = nullptr;
    Span<const GiLight> lights;
    /// The player's region, or any region a designer marked important. Probes are placed at full
    /// density inside it whatever the geometry says.
    Aabb importance_region{};
    bool has_importance_region = false;
    /// The placed GI volumes, or null. Their probe density decides acceptance where they reach, and
    /// what they resolve to is recorded on each probe placed.
    const GiVolumeSet* volumes = nullptr;
};

class RadianceCache;

/// One probe's gather, as a backend hands it back. `payload` is
/// `encoding_traits().floats_per_probe` floats in the cache's encoding; the rest is what
/// `gather_probe` writes on the host.
struct ProbeGatherResult {
    u32 probe = 0;
    /// The probe's revision when its gather was submitted. A result whose revision no longer
    /// matches is dropped: the probe was invalidated, retired or reseeded while it was in flight.
    u32 revision = 0;
    Span<const f32> payload;
    f32 axis_distance[6] = {};
    f32 error = 0.0F;
    u64 frame = 0;
};

/// `RadianceCache::sample_traced`'s two answers.
struct TracedCacheSamples {
    RadianceSample diffuse{};
    RadianceSample specular{};
};

/// WHO GATHERS THE PROBES THE SCHEDULER PICKED. The seam a device radiance cache replaces the host
/// gather behind (`cy::rendering-gi-gpu`), exactly as `SurfaceShadingBackend` is for the surface
/// cache: the cache keeps every DECISION — placement, the clipmap scroll, invalidation, the
/// prioritised, budgeted, progress-guaranteeing selection (`RadianceCache::select`) and the query
/// a resolve reads — and hands a backend only the rays.
///
/// Asynchronous by construction. `submit` takes this update's selection; the work runs when the
/// caller executes its frame; `retire` hands the results back through `RadianceCache::apply` at the
/// start of the next update, so the caller's contract is that the frame that ran the previous
/// submission has completed by then.
class ProbeGatherBackend {
public:
    ProbeGatherBackend() = default;
    ProbeGatherBackend(const ProbeGatherBackend&) = delete;
    ProbeGatherBackend(ProbeGatherBackend&&) = delete;
    ProbeGatherBackend& operator=(const ProbeGatherBackend&) = delete;
    ProbeGatherBackend& operator=(ProbeGatherBackend&&) = delete;
    virtual ~ProbeGatherBackend() = default;

    /// Take this update's selection: `selected` is probe handles in priority order, no longer than
    /// the budget. `cache` is everything the backend may mirror — the probes, their payloads, the
    /// settings and the clipmap's centre. Returns how many it accepted: a backend sized for fewer
    /// probes than the budget takes a prefix, which is still the highest-priority work.
    [[nodiscard]] virtual u32 submit(const RadianceCache& cache, Span<const u32> selected,
                                     const ProbeUpdateContext& context) noexcept = 0;

    /// Hand the results of the last submission to `cache.apply()`. Returns how many it handed.
    virtual u32 retire(RadianceCache& cache) noexcept = 0;
};

/// World-space probes storing incoming radiance.
class RadianceCache : public IndirectSource {
public:
    RadianceCache() noexcept;
    ~RadianceCache() override;

    RadianceCache(const RadianceCache&) = delete;
    RadianceCache(RadianceCache&&) = delete;
    RadianceCache& operator=(const RadianceCache&) = delete;
    RadianceCache& operator=(RadianceCache&&) = delete;

    [[nodiscard]] Status configure(const ProbeCacheSettings& settings) noexcept;
    [[nodiscard]] const ProbeCacheSettings& settings() const noexcept { return settings_; }

    /// Centre the clipmaps on `camera` and populate the newly exposed regions. Probes already
    /// present and still in range are reused untouched — which is what makes a camera translation
    /// cost the new region rather than the world.
    ProbePlacementReport scroll_to(Vec3 camera, const ProbePlacementContext& context) noexcept;

    /// Invalidate the probes inside `region`. Returns how many, which fills in an invalidation
    /// record's `probes`.
    u32 invalidate(const Aabb& region) noexcept;

    /// Retire the probes inside `region`, so the next `scroll_to` places that region again. What a
    /// change to the GI volumes does to the regions it touched: their density and the settings
    /// recorded on each probe are decided at placement. Returns how many.
    u32 retire_region(const Aabb& region) noexcept;

    ProbeUpdateReport update(const ProbeUpdateContext& context) noexcept;

    // --- Queries ---------------------------------------------------------------------------------

    /// The irradiance arriving at a point, interpolated over the nearby probes and weighted by the
    /// visibility term so a probe behind a wall contributes nothing.
    [[nodiscard]] Vec3 gather(Vec3 position, Vec3 normal) const noexcept override;

    /// The same, with the confidence the resolve needs: it falls with probe staleness, with how
    /// much probe weight the query actually found, and to zero where no probe covers the point.
    [[nodiscard]] RadianceSample sample(Vec3 position, Vec3 normal, u64 frame) const noexcept;

    /// The cache at a surface for the two signals a per-pixel resolve takes — diffuse about the
    /// surface's `normal`, specular about its `reflection` — with every probe that would contribute
    /// TESTED FOR VISIBILITY THROUGH `field`: a sphere trace from the surface, lifted along its
    /// normal by the visibility bias, to the probe; a probe the trace stops short of contributes
    /// to neither. One trace a probe answers both signals.
    ///
    /// The six axis distances are a probe's view of the world averaged over a hemisphere each, and
    /// a thin wall a metre from a probe is averaged with the far wall behind it: `sample` then lets
    /// a probe in a lit room light a point diagonally beyond a 10 cm wall in the dark one. A
    /// lobe-averaged depth map per probe has the same failure at a grazing wall (measured while
    /// building issue #35's stage 4). The trace does not. Probes within ONE spacing contribute
    /// rather than `sample`'s one and a half, because each now costs a trace: about four a level
    /// rather than about fourteen. `sample` itself is unchanged for every other consumer.
    [[nodiscard]] TracedCacheSamples sample_traced(Vec3 position, Vec3 normal, Vec3 reflection,
                                                   u64 frame,
                                                   const DistanceField& field) const noexcept;

    /// Write a probe's payload directly. What a bake's seeding uses, and the only way a probe
    /// becomes valid without a gather.
    [[nodiscard]] Status seed(u32 probe, Span<const Vec3> directions,
                              Span<const Vec3> radiance) noexcept;

    // --- The device seam
    // --------------------------------------------------------------------------

    /// The prioritised selection one update gathers, in order: every live probe older than
    /// `context.max_age_frames` first (the progress guarantee), then by priority, then by handle.
    /// `context.converged` takes every live probe and otherwise the first `context.budget`. Writes
    /// `queue_depth` and `oldest_unserviced_age` into `report`. A static function over the probe
    /// array, so a device backend and a test holding one to its budget run the one selection there
    /// is — lifted out of `update()` unchanged.
    [[nodiscard]] static Status select(Span<const Probe> probes, const ProbeUpdateContext& context,
                                       Array<u32>& selected, ProbeUpdateReport& report) noexcept;

    /// Gather the selected probes somewhere else. Null restores the host gather. Installing one
    /// touches no probe. See `ProbeGatherBackend`.
    void set_gather_backend(ProbeGatherBackend* backend) noexcept { backend_ = backend; }
    [[nodiscard]] ProbeGatherBackend* gather_backend() const noexcept { return backend_; }

    /// Fold the backend's completed results into the probes now, rather than at the next update.
    /// A no-op with no backend. Returns how many probes it wrote.
    u32 collect() noexcept;

    /// A backend's door back in: write one gathered probe. Refused — returns false and writes
    /// nothing — for a probe that is no longer live or whose revision moved since the gather was
    /// submitted, and for a payload of the wrong length.
    bool apply(const ProbeGatherResult& result) noexcept;

    /// One probe's payload, `encoding_traits(settings().encoding).floats_per_probe` floats.
    [[nodiscard]] Span<const f32> probe_payload(u32 probe) const noexcept;

    /// Bumped by every `configure`, which starts the probe slots — and so their revisions — over. A
    /// device mirror that keyed its uploads on revisions alone would take a new probe in slot 0 for
    /// the old one; it starts over when this moves.
    [[nodiscard]] u64 generation() const noexcept { return generation_; }

    /// The camera the clipmaps were last centred on. A device mirror of the probe layout addresses
    /// each level's window from it.
    [[nodiscard]] Vec3 camera() const noexcept { return camera_; }

    /// The spacing of one level's probes, in metres.
    [[nodiscard]] f32 level_spacing(u32 level) const noexcept {
        return settings_.base_spacing_metres * static_cast<f32>(1U << level);
    }

    [[nodiscard]] u32 probe_count() const noexcept { return live_probes_; }
    [[nodiscard]] const Probe& probe(u32 index) const noexcept { return probes_[index]; }
    [[nodiscard]] Span<const Probe> probes() const noexcept { return probes_.span(); }
    [[nodiscard]] const RadianceCacheDiagnostics& diagnostics() const noexcept {
        return diagnostics_;
    }

    /// The mean absolute change the last update made, over the probes it touched. The convergence
    /// signal `resolve.h` reads: it falls toward zero as the cache settles.
    [[nodiscard]] f32 mean_error() const noexcept { return diagnostics_.last_update.mean_error; }

private:
    [[nodiscard]] f32* payload(u32 probe) noexcept;
    [[nodiscard]] const f32* payload(u32 probe) const noexcept;
    [[nodiscard]] Expected<u32, Error> create_probe(Vec3 position, u32 level) noexcept;
    void retire_probe(u32 probe) noexcept;
    [[nodiscard]] bool accept_candidate(Vec3 position, u32 level, f32 spacing,
                                        const ProbePlacementContext& context,
                                        ProbePlacementReport& report) const noexcept;
    [[nodiscard]] bool accept_by_geometry(Vec3 position, u32 level, f32 spacing, f32 distance,
                                          const ProbePlacementContext& context,
                                          bool sparse_only) const noexcept;
    void retire_outside_window(Vec3 camera, i32 half, ProbePlacementReport& report) noexcept;
    void populate_level(u32 level, Vec3 camera, i32 half, const ProbePlacementContext& context,
                        ProbePlacementReport& report) noexcept;
    void gather_probe(u32 handle, const ProbeUpdateContext& context) noexcept;
    ProbeUpdateReport submit_to_backend(Span<const u32> selected,
                                        const ProbeUpdateContext& context) noexcept;
    [[nodiscard]] static f32 priority_of(const Probe& entry, u64 frame) noexcept;
    /// The probe's age as the scheduler sees it: frames since its update, times its update rate.
    [[nodiscard]] static f32 scheduled_age(const Probe& entry, u64 frame) noexcept;
    [[nodiscard]] f32 visibility_weight(const Probe& entry, Vec3 query) const noexcept;
    void account() noexcept;

    ProbeCacheSettings settings_{};
    Array<Probe> probes_;
    Array<f32> payloads_;
    Array<u32> proxies_;
    Array<u32> free_probes_;
    /// (level, cell) to probe. The clipmap's identity, and what makes a scroll a reuse.
    HashMap<u64, u32> occupancy_;
    DynamicBvh index_;
    ProbeGatherBackend* backend_ = nullptr;
    u32 live_probes_ = 0;
    u32 stride_ = 0;
    u64 generation_ = 0;
    Vec3 camera_{0.0F, 0.0F, 0.0F};
    mutable RadianceCacheDiagnostics diagnostics_{};
};

// --- The encodings, exposed so a test can check one against another ------------------------------

/// Fold one gathered sample into a probe's payload.
void encode_sample(ProbeEncoding encoding, f32* payload, Vec3 direction, Vec3 radiance,
                   f32 weight) noexcept;

/// Finish an encoding after `sample_count` samples have been folded in.
void normalise_payload(ProbeEncoding encoding, f32* payload, f32 total_weight,
                       u32 sample_count) noexcept;

/// Evaluate the irradiance a payload carries in one direction.
[[nodiscard]] Vec3 decode_payload(ProbeEncoding encoding, const f32* payload,
                                  Vec3 direction) noexcept;

/// The direction of a probe's `index`-th gather ray out of `count`, in the frame's `rotation`: the
/// golden-ratio sphere sequence `gather_probe` traces. Public because a device gather traces the
/// same rays — the directions are uniform over probes, so a backend computes the table once per
/// update rather than per probe, and both sides then trace bit-identical directions.
[[nodiscard]] Vec3 probe_ray_direction(u32 index, u32 count, u32 rotation) noexcept;

/// Which subset of the sphere sequence a frame takes. Rays are distributed across frames rather
/// than resolved fully per frame; this is the rotation `gather_probe` uses for `frame`.
[[nodiscard]] constexpr u32 probe_ray_rotation(u64 frame) noexcept {
    return static_cast<u32>(frame & 0xFFU);
}

}  // namespace cy::rendering::gi
