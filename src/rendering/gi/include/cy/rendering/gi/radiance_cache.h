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
};

/// What the placement pass needs in order to decide where a probe buys something.
struct ProbePlacementContext {
    const DistanceField* field = nullptr;
    Span<const GiLight> lights;
    /// The player's region, or any region a designer marked important. Probes are placed at full
    /// density inside it whatever the geometry says.
    Aabb importance_region{};
    bool has_importance_region = false;
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

    ProbeUpdateReport update(const ProbeUpdateContext& context) noexcept;

    // --- Queries ---------------------------------------------------------------------------------

    /// The irradiance arriving at a point, interpolated over the nearby probes and weighted by the
    /// visibility term so a probe behind a wall contributes nothing.
    [[nodiscard]] Vec3 gather(Vec3 position, Vec3 normal) const noexcept override;

    /// The same, with the confidence the resolve needs: it falls with probe staleness, with how
    /// much probe weight the query actually found, and to zero where no probe covers the point.
    [[nodiscard]] RadianceSample sample(Vec3 position, Vec3 normal, u64 frame) const noexcept;

    /// Write a probe's payload directly. What a bake's seeding uses, and the only way a probe
    /// becomes valid without a gather.
    [[nodiscard]] Status seed(u32 probe, Span<const Vec3> directions,
                              Span<const Vec3> radiance) noexcept;

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
    void retire_outside_window(Vec3 camera, i32 half, ProbePlacementReport& report) noexcept;
    void populate_level(u32 level, Vec3 camera, i32 half, const ProbePlacementContext& context,
                        ProbePlacementReport& report) noexcept;
    void gather_probe(u32 handle, const ProbeUpdateContext& context) noexcept;
    [[nodiscard]] static f32 priority_of(const Probe& entry, u64 frame) noexcept;
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
    u32 live_probes_ = 0;
    u32 stride_ = 0;
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

}  // namespace cy::rendering::gi
