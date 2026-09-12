#pragma once
// The shoreline, which water OWNS, and the environment fields it writes. M10 task 2.3.
//
// `water` — "Shoreline": "The shoreline SHALL be computed jointly from terrain and water: water
// level and wave approach against terrain height, slope, and material. Shoreline computation SHALL
// be OWNED BY WATER, which reads terrain and writes WETNESS, WATER DISTANCE, and WATER DEPTH into
// environment fields. Terrain SHALL NOT query the water system directly; it SHALL read those
// fields. This dependency direction SHALL be maintained so that terrain and water do not become
// mutually dependent."
//
// ================================================================================================
// THE TERRAIN SEAM IS ONE FUNCTION POINTER, AND THAT IS THE ACYCLICITY
// ================================================================================================
//
// What water needs from terrain is one number: the height of the ground under a point. `BedSource`
// is that number as a callback, installed by the application. src/terrain/ does not exist on this
// tree and this module would still not name it if it did — see the module's CMakeLists, where the
// absence of `cy::terrain` from the dependency list is the requirement made checkable by the link
// graph rather than by a review.
//
// The other direction is fields: terrain materials, foliage, audio and navigation read
// `water-distance`, `water-depth`, `water-flow` and `wetness` from `environment-fields` and never
// call here. That is the specification's "The dependency stays acyclic" scenario, and it is why
// this file's only outbound dependency is the substrate.
//
// ================================================================================================
// WETNESS HAS TWO POSSIBLE PRODUCERS, AND THE SUBSTRATE ALLOWS ONE
// ================================================================================================
//
// `water` says the shoreline writes wetness. `weather-and-wind` says precipitation accumulates into
// wetness. `environment-fields` says a field has exactly one producer, refused at registration.
// Three requirements, and on a world with both rows live, two of them cannot both be satisfied by
// claiming the same field.
//
// So it is DECLARED, not discovered: `WetnessOwner` says which row owns `wetness` in this
// configuration. With `Water`, this module claims it and the specification's "A wet shore" scenario
// is literally true. With `External`, this module claims `water-shore-wetness` instead — a field of
// its own, carrying the shore's contribution — and the wetness producer composes it with
// precipitation. Both are honest; what would not be is claiming `wetness` and letting whichever row
// registered second take the refusal as a surprise.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/water/query.h>

namespace cy::water {

/// THE TERRAIN SEAM. The height of the ground at a horizontal position, in absolute metres.
///
/// A callback and not an interface class, for the reason `environment::TileLoader` is one: the
/// producer on the other side is a row that does not exist yet, and a virtual base here would be a
/// base that row has to inherit rather than a function it has to supply.
using BedSource = f64 (*)(void* user, f64 x, f64 z) noexcept;

/// What the shoreline reads. Two callbacks: the water's own answer, and the terrain's bed.
///
/// The water side is a callback rather than a `const WaterSystem&` so that this file does not have
/// to include system.h — which includes this one. It is filled in by `WaterSystem::shoreline()`,
/// and a test can fill it with a flat sea and a sloping beach and check the fields without a system
/// at all, which is exactly what the suite does.
struct ShorelineInputs {
    using WaterAt = void (*)(void* user, const world::WorldVec3d& at, WaterSample& out) noexcept;

    WaterAt water_at = nullptr;
    void* water_user = nullptr;
    BedSource bed_at = nullptr;
    void* bed_user = nullptr;

    /// How far up the shore the water reaches beyond its own edge, in metres — the wave run-up.
    /// Derived by `WaterSystem` from the authoritative band amplitudes, because the run-up of a
    /// calm lake and of a rough sea differ by exactly that.
    f32 run_up_metres = 1.0F;

    [[nodiscard]] bool complete() const noexcept { return water_at != nullptr; }
};

/// Which row produces `wetness` in this configuration. See the header note.
enum class WetnessOwner : u8 {
    /// Water claims `wetness` itself. The specification's "A wet shore" scenario, literally.
    Water = 0,
    /// Another row — `weather-and-wind` — produces `wetness`, and water produces
    /// `water-shore-wetness` for it to compose.
    External = 1,
};

[[nodiscard]] const char* wetness_owner_name(WetnessOwner owner) noexcept;

/// The name of the field water produces for the shore's wetness contribution when it does not own
/// `wetness`. Declared here so the composing row names one string rather than agreeing on one.
inline constexpr const char* kShoreWetnessField = "water-shore-wetness";

/// How the four fields are declared. Ranges matter: they are the storage contract of a quantised
/// field, so widening one changes what every stored byte means.
struct WaterFieldOptions {
    WetnessOwner wetness_owner = WetnessOwner::Water;

    /// The three declared resolutions, in metres per cell. The macro level is resident everywhere —
    /// it has to be, because these fields are gameplay-visible and a gameplay sample reads one
    /// declared level whatever streamed.
    f32 local_cell_metres = 2.0F;
    f32 regional_cell_metres = 8.0F;
    f32 macro_cell_metres = 64.0F;

    /// The furthest water distance the field represents. Beyond it the field saturates, which is
    /// the correct behaviour for a quantity whose consumers — a wetness ramp, a foliage rule, an
    /// audio emitter — stop caring long before the far edge.
    f32 max_water_distance_metres = 256.0F;
    f32 max_water_depth_metres = 64.0F;
    f32 max_flow_mps = 12.0F;
    /// How far inland, in metres beyond the water's edge and the run-up, the shore is damp.
    f32 wet_band_metres = 3.0F;
};

/// The four fields, their declarations, their producer tokens, and the publication that fills them.
///
/// **This is a PRODUCER into `environment-fields` and holds no store of its own.** Everything it
/// computes lands in `FieldStore` tiles through a `FieldWriter`, which is what makes "terrain
/// materials, foliage, audio and gameplay read them without querying the water system" true.
class WaterFields {
public:
    explicit WaterFields(Allocator& allocator) noexcept;

    WaterFields(const WaterFields&) = delete;
    WaterFields& operator=(const WaterFields&) = delete;

    /// Declare the fields into a registry. Idempotent: re-declaring identically is accepted by the
    /// substrate, so two modules that both need `water-depth` to exist need not agree on which of
    /// them declares it.
    [[nodiscard]] Status declare(environment::FieldRegistry& registry,
                                 const WaterFieldOptions& options) noexcept;

    /// Claim the producer tokens. **This is where a second producer for one of these fields is
    /// refused**, by the substrate, naming both — so a configuration in which weather also claims
    /// `wetness` fails here with a message that says so rather than racing at run time.
    [[nodiscard]] Status claim(environment::FieldRegistry& registry,
                               environment::FieldStore& store) noexcept;

    /// Compute the shoreline over a rectangle of world and publish it into the fields, at one
    /// declared level.
    ///
    /// The rectangle is the caller's: a streamer publishes the cells that became resident, an
    /// editor publishes what a designer is looking at, and a cook publishes the world. This module
    /// does not decide how much world to write, because the answer differs for all three.
    [[nodiscard]] Status publish(const ShorelineInputs& inputs, environment::FieldResidency level,
                                 f64 min_x, f64 min_z, f64 max_x, f64 max_z) noexcept;

    [[nodiscard]] environment::FieldId depth_field() const noexcept { return depth_; }
    [[nodiscard]] environment::FieldId distance_field() const noexcept { return distance_; }
    [[nodiscard]] environment::FieldId flow_field() const noexcept { return flow_; }
    /// `wetness`, or `water-shore-wetness` — whichever this configuration declared water produces.
    [[nodiscard]] environment::FieldId wetness_field() const noexcept { return wetness_; }
    [[nodiscard]] WetnessOwner wetness_owner() const noexcept { return options_.wetness_owner; }
    [[nodiscard]] bool claimed() const noexcept { return claimed_; }
    /// Lattice points written by the last publish. What a test compares against the rectangle, and
    /// what a profiler view reports.
    [[nodiscard]] u64 last_published_points() const noexcept { return published_points_; }

private:
    /// The four writers one publication holds open, as one thing to pass around. Pointers rather
    /// than values because `environment::FieldWriter` is move-only and the writers live for the
    /// whole of `write_tiles()`, which is also where their `publish()` happens.
    struct ShoreWriters {
        environment::FieldWriter* depth = nullptr;
        environment::FieldWriter* distance = nullptr;
        environment::FieldWriter* flow = nullptr;
        environment::FieldWriter* wetness = nullptr;
    };

    /// One lattice point of the rectangle being published, before it becomes four field values.
    struct ShorePoint {
        f64 bed = 0.0;
        f64 surface = 0.0;
        f32 depth = 0.0F;
        f32 flow_x = 0.0F;
        f32 flow_z = 0.0F;
        /// Distance to the nearest water, metres, filled by the sweep. Zero in water.
        f32 distance = 0.0F;
        bool in_water = false;
    };

    [[nodiscard]] Status sample_rectangle(const ShorelineInputs& inputs, f32 cell_metres, i64 min_i,
                                          i64 min_k, u32 width, u32 height) noexcept;
    /// The two-pass chamfer sweep that turns "which points are water" into a distance in metres.
    /// The two passes are separate functions because they are separate claims: the forward one
    /// finds water above and to the left, the backward one water below and to the right, and a
    /// sweep missing either is correct only for a shore that happens to face the right way.
    void sweep_distances(u32 width, u32 height, f32 cell_metres) noexcept;
    void sweep_forward(u32 width, u32 height, f32 straight, f32 diagonal) noexcept;
    void sweep_backward(u32 width, u32 height, f32 straight, f32 diagonal) noexcept;
    /// One relaxation: a candidate distance through a neighbour, taken when it is shorter.
    void relax(usize target, usize source, f32 step) noexcept;
    [[nodiscard]] Status write_tiles(environment::FieldResidency level, i64 min_i, i64 min_k,
                                     u32 width, u32 height, const ShorelineInputs& inputs) noexcept;
    /// One lattice point of the rectangle, staged into all four fields. Separate from the loop
    /// because the tile arithmetic and the four values are what a reader checks against
    /// `environment::store.cpp`'s own lattice convention, and they should be readable without the
    /// writers' lifetime around them.
    [[nodiscard]] Status write_point(const ShoreWriters& writers, environment::FieldResidency level,
                                     i64 lattice_x, i64 lattice_z, const ShorePoint& point,
                                     f32 wet_band) noexcept;

    Allocator* allocator_;
    WaterFieldOptions options_;
    environment::FieldStore* store_ = nullptr;

    environment::FieldId depth_;
    environment::FieldId distance_;
    environment::FieldId flow_;
    environment::FieldId wetness_;

    environment::ProducerToken depth_token_;
    environment::ProducerToken distance_token_;
    environment::ProducerToken flow_token_;
    environment::ProducerToken wetness_token_;

    /// The rectangle being published, reused between publications so a steady-state publish of one
    /// cell allocates nothing.
    Array<ShorePoint> points_;
    u64 published_points_ = 0;
    bool declared_ = false;
    bool claimed_ = false;
};

}  // namespace cy::water
