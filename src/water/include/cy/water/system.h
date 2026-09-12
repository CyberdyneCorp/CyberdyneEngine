#pragma once
// CyberWater's front door: one system, one query, and the seams to weather, navigation and
// persistence. M10 task 2.3.
//
// Everything a consumer of water touches is here. The parts are `body.h`'s registry,
// `displacement.h`'s contract, `ocean.h`'s cascades, `river.h`'s network, `foam.h`'s field,
// `shoreline.h`'s fields and `streaming.h`'s segments; what this file adds is the composition of
// them, and the three requirements that are about the composition rather than about any part:
//
//   * **One query.** `water` — "every consumer SHALL query them through ONE INTERFACE". A caller
//     never learns which backend a body has, and `query()` returns the authoritative bands always,
//     for everyone, so a renderer that asks this question gets the physics answer. The renderer's
//     own extra bands come from `OceanSurface`, which is the only thing in this module that
//     evaluates `BandSelection::All`.
//
//   * **Overlap resolves to one answer.** A river mouth is inside an ocean's bounds. The declared
//     order decides, and the rule is stated in `query()`: the highest-priority body that actually
//     HAS water at the position answers. Outside the channel the sea answers; inside it the river
//     does; and the answer does not depend on which of them was registered first.
//
//   * **Weather is a seam, not a dependency.** `drive_ocean_from_wind()` takes a wind vector — the
//     one a consumer read from the `wind` field, whose producer is `weather-and-wind`. Water does
//     not sample the field itself, so a project with no weather passes an authored vector and the
//     sea works, which is the specification's "Water works without weather" scenario as a
//     signature rather than as a promise.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/water/body.h>
#include <cy/water/buoyancy.h>
#include <cy/water/displacement.h>
#include <cy/water/foam.h>
#include <cy/water/ocean.h>
#include <cy/water/query.h>
#include <cy/water/river.h>
#include <cy/water/shading.h>
#include <cy/water/shoreline.h>
#include <cy/water/streaming.h>

namespace cy::water {

/// Per-body diagnostics. `water` — "The engine SHALL report: bodies and their backends, resident
/// segments, simulation cost per body, displacement band amplitudes with the authoritative and
/// visual split, query counts and cost, foam field memory, and buoyancy sample counts."
struct WaterBodyDiagnostics {
    WaterBodyId id;
    const char* name = "";
    WaterBodyType type = WaterBodyType::Lake;
    WaterBackend backend = WaterBackend::Flat;
    u32 resident_segments = 0;
    /// Wave trains summed for this body's queries since the last reset. The simulation cost per
    /// body, counted rather than estimated: a body with four cascades of four trains costs sixteen
    /// evaluations per query and this is that number, actual.
    u64 simulation_cost = 0;
    u64 queries = 0;
    AmplitudeSplit split;
};

/// System-wide diagnostics.
struct WaterDiagnostics {
    u32 bodies = 0;
    u32 resident_segments = 0;
    u64 segment_bytes = 0;
    u64 foam_bytes = 0;
    /// Queries answered, and wave trains summed for them. The ratio is the query cost.
    u64 queries = 0;
    u64 query_cost = 0;
    /// Queries that fell back to a body's mean level because nothing was resident there.
    u64 mean_level_queries = 0;
    u64 buoyancy_samples = 0;
    u64 buoyancy_solves = 0;
};

/// A region whose navigation data water has invalidated. `water` — "Water level changes and
/// flooding SHALL emit navigation dirty regions like terrain change does."
struct WaterDirtyRegion {
    WaterBodyId body;
    f64 min_x = 0.0;
    f64 min_z = 0.0;
    f64 max_x = 0.0;
    f64 max_z = 0.0;
    /// The level change that caused it, metres. Signed: a flood and a drought invalidate the same
    /// rectangle and mean opposite things to whatever rebuilds it.
    f32 level_delta = 0.0F;
};

/// The costs navigation asks water for. `water` — "Water SHALL contribute to navigation: swimming
/// volumes for submerged navigation, a surface for vessel navigation, and DIRECTIONAL COST on
/// flowing water so that upstream travel costs more than downstream."
struct WaterNavigationCosts {
    /// Multiplier for swimming through water, against dry ground at 1.
    f32 swim_multiplier = 2.5F;
    /// Multiplier for a vessel on the surface.
    f32 surface_multiplier = 0.8F;
    /// How much the flow helps or hinders, as a fraction of the base cost at a flow of
    /// `reference_flow_mps` directly with or against the direction of travel.
    f32 flow_influence = 0.6F;
    f32 reference_flow_mps = 3.0F;
    /// The floor a cost may not go below however helpful the current is. Without it a fast enough
    /// river makes downstream travel free, and a zero-cost edge breaks every path search that
    /// assumes positive weights.
    f32 minimum_multiplier = 0.2F;
};

/// The whole of water, composed.
class WaterSystem {
public:
    WaterSystem(Allocator& allocator, const world::PartitionConfig& partition) noexcept;

    WaterSystem(const WaterSystem&) = delete;
    WaterSystem& operator=(const WaterSystem&) = delete;

    [[nodiscard]] WaterRegistry& registry() noexcept { return registry_; }
    [[nodiscard]] const WaterRegistry& registry() const noexcept { return registry_; }
    [[nodiscard]] RiverNetwork& rivers() noexcept { return rivers_; }
    [[nodiscard]] const RiverNetwork& rivers() const noexcept { return rivers_; }
    [[nodiscard]] FoamField& foam() noexcept { return foam_; }
    [[nodiscard]] const FoamField& foam() const noexcept { return foam_; }
    [[nodiscard]] WaterFields& fields() noexcept { return fields_; }
    [[nodiscard]] WaterStreaming& streaming() noexcept { return streaming_; }
    [[nodiscard]] const WaterStreaming& streaming() const noexcept { return streaming_; }

    /// Give a body a spectral model. Refuses a body that is not registered, a body whose backend is
    /// not `Spectral`, and a model that fails the displacement contract — the refusal is
    /// `validate_model()`'s own problem code, so a visual band large enough to be felt by gameplay
    /// is refused HERE rather than discovered by a boat.
    [[nodiscard]] Status set_ocean(WaterBodyId body, const OceanParams& params, u64 seed) noexcept;
    [[nodiscard]] Status set_ocean(WaterBodyId body, const OceanParams& params, u64 seed,
                                   OceanReport& report) noexcept;

    /// THE WEATHER SEAM. Re-derive a body's spectrum from a wind vector — the value a consumer read
    /// from the `wind` field. Keeps the body's other parameters, so a gust changes the sea and
    /// nothing else.
    [[nodiscard]] Status drive_ocean_from_wind(WaterBodyId body, const Vec3& wind_mps) noexcept;

    [[nodiscard]] const DisplacementModel* model_of(WaterBodyId body) const noexcept;
    [[nodiscard]] const OceanParams* ocean_params_of(WaterBodyId body) const noexcept;

    /// THE TERRAIN SEAM. See `shoreline.h`: one callback, and no dependency on a terrain module.
    void set_bed_source(BedSource source, void* user) noexcept;

    /// Advance the simulation clock and the foam field. `focus` is where the foam grid is centred —
    /// the camera, or the nearest streaming source.
    [[nodiscard]] Status tick(f32 seconds, const world::WorldVec3d& focus) noexcept;
    [[nodiscard]] f64 time() const noexcept { return time_; }
    void set_time(f64 seconds) noexcept { time_ = seconds; }

    // --- Queries ------------------------------------------------------------------------------

    /// The water at a position. Never blocks, never allocates, never calls into physics.
    [[nodiscard]] WaterSample query(const world::WorldVec3d& at) const noexcept;

    /// The batched form. One resolution of the body order and one model lookup for the whole batch
    /// where every position falls in one body, which is the per-sample overhead the requirement is
    /// about — "WHEN a vessel samples its hull at forty points, THEN the query SHALL be batchable".
    [[nodiscard]] Status query_many(Span<const world::WorldVec3d> positions,
                                    Span<WaterSample> out) const noexcept;

    /// The diagnostic point query: which body owns it, the authoritative height, and which bands
    /// contributed. `bands` receives up to `kMaxDisplacementBands` contributions.
    [[nodiscard]] Status point_report(const world::WorldVec3d& at, WaterPointReport& report,
                                      BandContribution* bands) const noexcept;

    /// Buoyancy for a hull. Queries the water at each sample itself, so a caller cannot
    /// accidentally float a boat on a set of samples taken with the visual bands — there is no
    /// overload that takes them.
    [[nodiscard]] Expected<BuoyancyResult, Error> buoyancy(const BuoyancyState& state,
                                                           Span<const BuoyancySample> samples,
                                                           const BuoyancyParams& params) noexcept;

    [[nodiscard]] CharacterWaterState character_state_at(
        const world::WorldVec3d& at, const SwimThresholds& thresholds) const noexcept;

    // --- Shoreline, navigation and level changes ----------------------------------------------

    /// The inputs `WaterFields::publish()` reads: this system's query and the installed bed source.
    [[nodiscard]] ShorelineInputs shoreline_inputs() const noexcept;

    /// The cost multiplier for travelling from one position to another through water. Downstream is
    /// cheaper than upstream because the flow's component along the direction of travel is
    /// subtracted from the cost — the specification's "Downstream is cheaper" scenario, as
    /// arithmetic over the same flow field that carries the debris.
    [[nodiscard]] f32 directional_cost(const world::WorldVec3d& from, const world::WorldVec3d& to,
                                       const WaterNavigationCosts& costs) const noexcept;

    /// Change a body's mean level, and emit the navigation dirty region that follows. The flood.
    [[nodiscard]] Status set_mean_level(WaterBodyId body, f64 level) noexcept;

    /// Take the dirty regions emitted since the last drain. Moved out, not copied: a consumer that
    /// drains twice must not rebuild twice.
    [[nodiscard]] Status drain_navigation_dirty(Array<WaterDirtyRegion>& out) noexcept;

    // --- Diagnostics ---------------------------------------------------------------------------

    [[nodiscard]] WaterDiagnostics diagnostics() const noexcept;
    [[nodiscard]] Status body_diagnostics(Array<WaterBodyDiagnostics>& out) const noexcept;
    void reset_counters() noexcept;

private:
    /// One body's simulation state: its model, if it has one, and its parameters.
    struct BodyState {
        WaterBodyId id;
        DisplacementModel model;
        OceanParams params;
        bool has_model = false;
        mutable u64 queries = 0;
        mutable u64 cost = 0;
    };

    [[nodiscard]] BodyState* find_state(WaterBodyId body) noexcept;
    [[nodiscard]] const BodyState* find_state(WaterBodyId body) const noexcept;
    [[nodiscard]] Status ensure_state(WaterBodyId body) noexcept;

    /// Whether a body has water AT this position, which is what the overlap order selects on. A
    /// river has water only inside its channel; every other backend has water everywhere inside its
    /// own bounds.
    [[nodiscard]] bool provides_water(const WaterBodyRecord& record,
                                      const world::WorldVec3d& at) const noexcept;
    /// The sample for one body at one position, with residency already decided.
    [[nodiscard]] WaterSample sample_body(const WaterBodyRecord& record,
                                          const world::WorldVec3d& at,
                                          bool resident) const noexcept;
    [[nodiscard]] f64 bed_height(const world::WorldVec3d& at, f64 fallback) const noexcept;

    /// The velocity callback the foam field advects by. A static function with `this` as its user,
    /// because `FoamField` takes a function pointer rather than a reference to this module.
    static Vec3 foam_velocity(void* user, const world::WorldVec3d& at) noexcept;
    /// The water callback `ShorelineInputs` carries, for the same reason.
    static void shoreline_water(void* user, const world::WorldVec3d& at, WaterSample& out) noexcept;

    Allocator* allocator_;
    const world::PartitionConfig* partition_;
    WaterRegistry registry_;
    RiverNetwork rivers_;
    FoamField foam_;
    WaterFields fields_;
    WaterStreaming streaming_;

    Array<BodyState> states_;
    Array<WaterDirtyRegion> dirty_;

    BedSource bed_source_ = nullptr;
    void* bed_user_ = nullptr;
    f64 time_ = 0.0;

    /// Counters. Mutable because counting is not a change to what the system answers, and a
    /// diagnostic that forced every caller to hold a non-const system is a diagnostic nobody
    /// leaves on — the reasoning `environment::FieldStore` records for the same decision.
    mutable u64 queries_ = 0;
    mutable u64 query_cost_ = 0;
    mutable u64 mean_level_queries_ = 0;
    u64 buoyancy_samples_ = 0;
    u64 buoyancy_solves_ = 0;
};

}  // namespace cy::water
