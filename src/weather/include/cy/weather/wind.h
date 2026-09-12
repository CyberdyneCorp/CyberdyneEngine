#pragma once
// THE WIND FIELD, and the determinism line drawn THROUGH it. M10 task 3.1.
//
// `weather-and-wind` — "The wind field": weather "SHALL be the producer of the wind field defined
// in `environment-fields`, composing: prevailing wind from climate, regional wind from weather
// cells and storms, terrain influence (blocking, channelling, ridge acceleration), authored local
// volumes, and transient sources"; "Local wind volumes SHALL be supported — directional, vortex,
// radial blast, updraft, downdraft, and spline-following"; "Wind SHALL carry base velocity, gust,
// turbulence, and vertical components, so consumers can use the component they need"; "Every wind
// consumer — foliage, VFX, cloth, water, clouds, audio, and gameplay where it applies — SHALL
// sample this field. No subsystem SHALL implement its own wind."
//
// ================================================================================================
// ONE FIELD, TWO CLASSES — AND THE LINE IS IN THE VALUE, NOT IN THE READER
// ================================================================================================
//
// A gameplay-visible wind and a presentation-only gust are not the same thing under
// `simulation-and-determinism`, and pretending they are has two failure modes, both real:
//
//   * put EVERYTHING in one authoritative field, and the high-frequency detail a blade of grass
//     needs becomes authoritative state that has to be hashed, snapshotted and replicated;
//   * put everything in one presentation field, and a projectile reading the wind is a firewall
//     violation — the exact crossing `simulation-and-determinism` exists to prevent.
//
// So the composition produces THREE components and publishes TWO fields:
//
//   `wind`             AUTHORITATIVE. `base` + `gust`, m/s, world axes, with the vertical
//                      component in `.y`. Every term is a pure function of (session seed, weather
//                      state, storm state, terrain, declared volumes, tick) — including the gust,
//                      which is drawn from `determinism::RandomStream` and is therefore
//                      reproducible, replicable and hashable. A projectile, a sailing boat, a fire
//                      spread and a navigation cost read this.
//   `wind-turbulence`  PRESENTATION. The high-frequency residual: what makes a leaf flutter and a
//                      smoke plume curl. `determinism::may_read()` refuses it to an authoritative
//                      reader, at `FieldReader::open()` and again over the whole configuration in
//                      `FieldRegistry::validate()`.
//
// **AND THE AUTHORITATIVE HALF IS THE SAME NUMBER FOR BOTH READERS.** `WindSample::authoritative()`
// is `base + gust` whoever asked; `sample()` fills `turbulence` only for a reader the firewall lets
// read it. That is how "trees, smoke, water, and cloth SHALL sample one field and move
// consistently" and "visual weather detail SHALL NOT influence authoritative state" are the same
// mechanism rather than two competing ones — the tree and the projectile agree about the wind
// exactly, and disagree only about a residual the projectile is not allowed to see. `test_wind.cpp`
// measures that equality bit for bit.
//
// ================================================================================================
// TRANSIENT SOURCES, THE BUDGET, AND WHY THE DROP IS DETERMINISTIC
// ================================================================================================
//
// `environment-fields` requires transient wind sources "registerable at runtime with a shape, a
// strength, and a lifetime", "bounded by a budget, with the lowest-priority sources dropped
// DETERMINISTICALLY when it is exceeded". The substrate left that to this row and named it as the
// debt.
//
// The drop is a total order over (priority descending, identity ascending) — NOT registration
// order, and not the order the array happens to be in. Two peers that registered the same sources
// in opposite orders keep the same set, which is what "deterministically" has to mean for a value
// gameplay can see; `test_wind.cpp` registers a set forwards and backwards and requires the kept
// sets to be identical.
//
// **What is deterministic is the SET THAT SURVIVES, not the sequence of drops that got there.**
// `last_dropped()` and `dropped_count()` are diagnostics: a peer that registered six sources one at
// a time and one that registered them in the other order reach the same three survivors by
// different routes, and only the survivors are read by anything. Saying so here rather than letting
// a reader assume the stronger claim is the difference between a guarantee and a hope.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/classification.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/weather/cells.h>
#include <cy/weather/climate.h>
#include <cy/world/coordinates.h>

namespace cy::weather {

class StormRegistry;

/// The shapes a local wind volume can have. The specification's own list, plus the spline the same
/// sentence names — "for tornadoes, ventilation, thrusters, explosions, and canyon wind".
enum class WindVolumeKind : u8 {
    /// A constant push inside the volume.
    Directional = 0,
    /// Rotation about an axis, with an inward and an upward component: a tornado, a dust devil.
    Vortex,
    /// Outward from a point, strongest at a moving shell: an explosion.
    RadialBlast,
    Updraft,
    Downdraft,
    /// Along a polyline: a canyon, a corridor, a ventilation duct.
    Spline,
};

[[nodiscard]] const char* wind_volume_kind_name(WindVolumeKind kind) noexcept;

/// How a volume's influence falls off from its centre or its axis.
enum class WindFalloff : u8 { Constant = 0, Linear, Smooth, InverseSquare };

/// The most control points one spline volume carries. A fixed array rather than an allocation: a
/// volume is a value a designer places, it is copied into the composer, and a canyon that needs
/// more than this is two volumes.
inline constexpr u32 kMaxWindSplinePoints = 8;

/// One local wind volume. Authored, or spawned at runtime as a transient source; the struct is the
/// same either way, because "a tornado is a volume" should not be a different type from "a canyon
/// is a volume".
struct WindVolume {
    WindVolumeKind kind = WindVolumeKind::Directional;
    WindFalloff falloff = WindFalloff::Smooth;
    /// Centre, absolute metres. For `Spline`, the first control point's frame of reference.
    world::WorldVec3d position;
    /// Metres. Outside it the volume contributes nothing at all, which is what keeps the
    /// composition's cost proportional to the volumes actually near the sample.
    f32 radius_metres = 30.0F;
    /// Metres per second at full influence.
    f32 strength = 10.0F;
    /// `Directional` and `Spline`: the direction, normalised by the caller. `Vortex`, `Updraft`
    /// and `Downdraft`: the axis.
    Vec3 direction{1.0F, 0.0F, 0.0F};
    /// `Vortex`: how much of the strength pulls inward rather than around, [0, 1].
    f32 inward = 0.2F;
    /// `Vortex`: how much lifts along the axis, [0, 1].
    f32 lift = 0.3F;
    /// `RadialBlast`: metres per second the shell travels outward. Zero makes it a steady radial.
    f32 shell_speed = 120.0F;
    /// `Spline`: the polyline, in absolute metres.
    world::WorldVec3d points[kMaxWindSplinePoints];
    u32 point_count = 0;
    /// Higher wins when the transient budget cannot hold every source.
    u16 priority = 0;

    [[nodiscard]] bool is_valid() const noexcept;
};

/// A volume's velocity at a position, and how much of it applies there. Free, so the composer, the
/// inspector and a test all evaluate one function.
[[nodiscard]] Vec3 wind_volume_velocity(const WindVolume& volume, const world::WorldVec3d& at,
                                        f32* influence_out) noexcept;

/// A transient source's identity. Supplied by the caller and STABLE — see the header note on the
/// budget: a drop ordered by identity is only deterministic if the identity is.
struct WindSourceId {
    u64 value = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(WindSourceId, WindSourceId) noexcept = default;
};

/// A runtime-registered volume with a lifetime. The explosion, the thruster, the passing vehicle.
struct TransientWindSource {
    WindSourceId id;
    WindVolume volume;
    /// Seconds. The source is dropped when its age reaches it; zero means "until removed".
    f32 lifetime_seconds = 2.0F;
    f32 age_seconds = 0.0F;
    /// Whether the strength fades over the lifetime. A blast does; a thruster does not.
    bool fade = true;

    [[nodiscard]] f32 remaining() const noexcept {
        return (lifetime_seconds <= 0.0F) ? 1.0F : (1.0F - (age_seconds / lifetime_seconds));
    }
};

/// Where a contribution came from. The inspector's requirement — "prevailing, storm, terrain, and
/// local contributions SHALL be shown separately alongside the result" — as an enumerator, so the
/// answer is a breakdown rather than a string.
enum class WindSourceKind : u8 {
    Prevailing = 0,
    Regional,
    Storm,
    Terrain,
    Volume,
    Transient,
    /// The deterministic gust. Authoritative, and reported separately because a designer asking
    /// "why is it gusting" is asking about this term and not about the mean.
    Gust,
    /// The presentation-only residual. Present in a breakdown only for a presentation reader.
    Turbulence,
    kCount,
};

inline constexpr u32 kWindSourceKindCount = static_cast<u32>(WindSourceKind::kCount);

[[nodiscard]] const char* wind_source_kind_name(WindSourceKind kind) noexcept;

struct WindContribution {
    WindSourceKind kind = WindSourceKind::Prevailing;
    Vec3 velocity;
    /// The volume or storm that produced it, where there is one. Zero otherwise.
    u64 source = 0;
};

/// One sampled wind, split into the three components the specification names plus the vertical,
/// which is a component of each of them rather than a fourth term.
struct WindSample {
    /// The mean flow: prevailing, regional, storm, terrain, volumes and transients. Authoritative.
    Vec3 base;
    /// The low-frequency modulation, drawn deterministically. Authoritative.
    Vec3 gust;
    /// The high-frequency residual. PRESENTATION ONLY, and zero for a reader the firewall does not
    /// let read it.
    Vec3 turbulence;

    /// What gameplay sees, and what every reader agrees about. See the header note.
    [[nodiscard]] Vec3 authoritative() const noexcept {
        return Vec3{base.x + gust.x, base.y + gust.y, base.z + gust.z};
    }
    /// What a renderer draws with.
    [[nodiscard]] Vec3 full() const noexcept {
        return Vec3{base.x + gust.x + turbulence.x, base.y + gust.y + turbulence.y,
                    base.z + gust.z + turbulence.z};
    }
    /// The vertical component of the authoritative wind, m/s, positive up.
    [[nodiscard]] f32 vertical() const noexcept { return base.y + gust.y; }
    [[nodiscard]] f32 speed() const noexcept;
};

/// The gust model. Declared rather than buried, because a gust that is authoritative state is a
/// gust whose parameters are part of the simulation's contract.
struct GustModel {
    /// Seconds of one gust cycle. The draw is indexed by `floor(t / period)` and interpolated
    /// across it, so the gust is continuous in time and still a pure function of the tick.
    f32 period_seconds = 7.0F;
    /// Metres of world over which the gust decorrelates. Two points closer than this gust together,
    /// which is what makes a gust look like a gust rather than like noise.
    f32 correlation_metres = 120.0F;
    /// Peak gust as a fraction of the mean wind speed.
    f32 strength = 0.45F;
    /// The vertical share of a gust, as a fraction of its horizontal magnitude.
    f32 vertical_share = 0.15F;
};

/// The presentation-only residual's model. Nothing here is part of authoritative state, and none of
/// these numbers may appear in a state hash.
struct TurbulenceModel {
    /// Metres of the finest eddy represented.
    f32 scale_metres = 6.0F;
    /// Peak turbulence as a fraction of the mean wind speed.
    f32 strength = 0.25F;
    /// How fast the pattern evolves, in pattern-lengths per second.
    f32 rate = 0.8F;
};

/// Composes the wind field from every source the specification lists.
///
/// It holds no field and writes none — `WeatherFields` (fields.h) publishes what this composes —
/// for the same reason `WeatherCells` does not: the model and the publication are separate things,
/// and a class that did both would make the producer rule a property of a method.
class WindComposer {
public:
    explicit WindComposer(Allocator& allocator) noexcept;

    WindComposer(const WindComposer&) = delete;
    WindComposer& operator=(const WindComposer&) = delete;

    /// The cells are where BOTH the regional term and the prevailing term come from: the prevailing
    /// wind is read from the climate the cell was seeded with, through
    /// `WeatherCells::climate_at()`, and never from the `ClimateMap` itself. See climate.h's header
    /// note — a composer that consulted the climate map per sample would be the recomputation the
    /// specification forbids.
    void set_cells(const WeatherCells* cells) noexcept { cells_ = cells; }
    void set_storms(const StormRegistry* storms) noexcept { storms_ = storms; }
    void set_terrain(const TerrainProfile& terrain) noexcept { terrain_ = terrain; }
    void set_gust_model(const GustModel& model) noexcept { gust_ = model; }
    void set_turbulence_model(const TurbulenceModel& model) noexcept { turbulence_ = model; }
    void set_seed(u64 seed) noexcept { seed_ = seed; }

    /// AUTHORED volumes: placed by a designer, living as long as the level does. Unbudgeted,
    /// because they are content rather than runtime pressure.
    [[nodiscard]] Status add_volume(const WindVolume& volume) noexcept;
    void clear_volumes() noexcept;
    [[nodiscard]] Span<const WindVolume> volumes() const noexcept { return volumes_.span(); }

    /// TRANSIENT sources: registered at runtime, with a lifetime, under a budget. Registering one
    /// beyond the budget is not an error — the lowest-priority source is dropped, deterministically
    /// (see the header note), and `last_dropped()` names it.
    [[nodiscard]] Status add_transient(const TransientWindSource& source) noexcept;
    [[nodiscard]] Status remove_transient(WindSourceId id) noexcept;
    void set_transient_budget(u32 budget) noexcept;
    [[nodiscard]] u32 transient_budget() const noexcept { return transient_budget_; }
    [[nodiscard]] Span<const TransientWindSource> transients() const noexcept {
        return transients_.span();
    }
    [[nodiscard]] WindSourceId last_dropped() const noexcept { return last_dropped_; }
    [[nodiscard]] u64 dropped_count() const noexcept { return dropped_; }

    /// Age the transient sources and retire the expired ones.
    [[nodiscard]] Status advance(f32 seconds) noexcept;

    /// The wind at a position, for a reader of the given class.
    ///
    /// `reader` decides ONLY whether the turbulence residual is filled in, through
    /// `determinism::may_read()` — the engine's own firewall predicate, not a second one. Every
    /// other term is identical for every reader, which is the requirement.
    [[nodiscard]] WindSample sample(const world::WorldVec3d& at,
                                    determinism::SimulationClass reader,
                                    determinism::SimulationPoint when, f64 seconds) const noexcept;

    /// "Sampling SHALL be batchable and SHALL NOT allocate." One pass over the positions with the
    /// weather cell, the storms and the volume list resolved once for the batch.
    [[nodiscard]] Status sample_many(Span<const world::WorldVec3d> positions,
                                     determinism::SimulationClass reader,
                                     determinism::SimulationPoint when, f64 seconds,
                                     Span<WindSample> out) const noexcept;

    /// The same sample, with every term reported separately. The inspector's answer; `out` is
    /// filled with at most its own length and the number written is returned.
    [[nodiscard]] usize explain(const world::WorldVec3d& at, determinism::SimulationClass reader,
                                determinism::SimulationPoint when, f64 seconds,
                                Span<WindContribution> out) const noexcept;

private:
    /// The composition, once, with each term handed to `emit`. `sample()` and `explain()` are two
    /// consumers of ONE walk — which is what stops the inspector from explaining a wind the sampler
    /// did not produce.
    struct Emitter {
        WindSample* sample = nullptr;
        Span<WindContribution> out;
        usize written = 0;
        void add(WindSourceKind kind, const Vec3& velocity, u64 source) noexcept;
    };

    void compose(const world::WorldVec3d& at, determinism::SimulationClass reader,
                 determinism::SimulationPoint when, f64 seconds, Emitter& emit) const noexcept;
    /// Terrain's three effects, as one term: blocking on the lee, channelling along a valley, and
    /// acceleration over a ridge.
    [[nodiscard]] Vec3 terrain_influence(const world::WorldVec3d& at,
                                         const Vec2& horizontal) const noexcept;
    [[nodiscard]] Vec3 gust_at(const world::WorldVec3d& at, determinism::SimulationPoint when,
                               f64 seconds, f32 mean_speed) const noexcept;
    [[nodiscard]] Vec3 turbulence_at(const world::WorldVec3d& at, f64 seconds,
                                     f32 mean_speed) const noexcept;
    /// Re-sort the transients into the total order the budget drops from, and drop the tail.
    [[nodiscard]] Status enforce_budget() noexcept;

    Allocator* allocator_;
    const WeatherCells* cells_ = nullptr;
    const StormRegistry* storms_ = nullptr;
    TerrainProfile terrain_;
    GustModel gust_;
    TurbulenceModel turbulence_;

    Array<WindVolume> volumes_;
    Array<TransientWindSource> transients_;
    u32 transient_budget_ = 32;
    WindSourceId last_dropped_;
    u64 dropped_ = 0;
    u64 seed_ = 0;
};

}  // namespace cy::weather
