#pragma once
// PRECIPITATION: what falls, how it is drawn, and how "am I under a roof" is answered without a ray
// per drop. M10 task 3.2.
//
// `weather-and-wind` — "Precipitation": weather "SHALL produce precipitation by type — rain, snow,
// hail, ash, dust, and project-defined — with an intensity published as field state"; "Presentation
// SHALL be tiered: effects near the camera, screen-space or volumetric approximation at middle
// distance, and field state alone at distance"; "**Individual precipitation particles SHALL NOT be
// physically simulated or collided.** Whether a position is sheltered SHALL be answered by a
// PRECIPITATION OCCLUSION REPRESENTATION — a coarse sky-visibility structure derived from scene
// geometry — not by a ray per drop"; "Interaction effects — splashes, drips, surface impacts —
// SHALL be driven by that occlusion and by the wetness field rather than by particle collision
// events."
//
// ================================================================================================
// THE OCCLUSION IS A GRID, AND ITS COST IS THE GRID'S
// ================================================================================================
//
// `SkyOcclusion` is a coarse raster over world x/z: per cell, the height of the lowest thing
// covering it and how much of the sky that thing hides. A position is sheltered when it is below
// its cell's cover height, and the answer costs one lookup whatever the rain rate is.
//
// It is BUILT from scene geometry — `add_cover()` takes a rectangle and a height, which is what a
// cooker walking roofs, bridges and canopies produces — rather than queried from it. Nothing here
// holds a scene, casts a ray or knows what a mesh is, and `queries()` counts the lookups so a suite
// can state the cost rather than describe it: `test_precipitation.cpp` raises the rain rate a
// hundredfold and requires the query count not to move.
//
// ================================================================================================
// THE TIERS ARE A PLAN, NOT A RENDERER
// ================================================================================================
//
// `PrecipitationPlan` says how many particles the near tier gets, that the middle tier is a
// screen-space or volumetric approximation, and that the far tier is field state alone. This module
// draws nothing: it produces the plan a presentation system consumes, in the same shape
// `rendering`'s budget levers take, so that reducing it is a lever rather than an edit. And
// **reducing it changes no number in `cells.h`** — the plan is computed FROM the state and nothing
// reads it back, which is the "Cost is bounded, state is not degraded" scenario made structural.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/weather/cells.h>
#include <cy/world/coordinates.h>

namespace cy::weather {

/// How precipitation of a type behaves, as data rather than as a switch statement. A project adding
/// a type adds a row; nothing in the engine branches on `PrecipitationType` except the table
/// lookup.
struct PrecipitationProperties {
    /// Metres per second of fall. Drives how far wind blows a drop sideways, which is what the
    /// presentation tier needs and the only reason this module knows it.
    f32 fall_speed = 6.0F;
    /// Fraction of the rate that becomes wetness rather than running off or bouncing.
    f32 wetness_yield = 1.0F;
    /// Millimetres of accumulated depth per millimetre per hour per second. Zero for everything
    /// that does not lie on the ground.
    f32 depth_yield = 0.0F;
    /// Metres of visibility removed at one millimetre per hour.
    f32 visibility_loss_per_mm = 900.0F;
    /// Whether the type is frozen, and therefore whether it accumulates as snow depth.
    bool frozen = false;
};

[[nodiscard]] PrecipitationProperties precipitation_properties(PrecipitationType type) noexcept;

/// Which type the conditions produce. Temperature decides rain against snow; the storm's type
/// decides hail, ash and dust, which is why it is passed in rather than inferred.
[[nodiscard]] PrecipitationType precipitation_type_for(f32 temperature_celsius,
                                                       PrecipitationType storm_type) noexcept;

// ================================================================================================
// THE OCCLUSION REPRESENTATION
// ================================================================================================

/// One cell of the occlusion raster.
struct SkyCoverCell {
    /// Metres above sea level of the lowest cover over this cell. A position below it is sheltered.
    f32 cover_height = 0.0F;
    /// How much of the sky that cover hides, [0, 1]. A dense canopy is not a roof.
    f32 occlusion = 0.0F;
};

/// What a shelter query answers.
struct ShelterSample {
    /// [0, 1]: 1 is open sky, 0 is fully covered. What a presentation system scales its effect by.
    f32 sky_visibility = 1.0F;
    /// True when the position is beneath cover at all.
    bool sheltered = false;
    /// Metres above the position of the cover, or zero where there is none. What a drip effect
    /// needs to know where to drip from.
    f32 cover_height_metres = 0.0F;
};

/// A coarse sky-visibility structure derived from scene geometry. See the header note.
///
/// SPARSE, because a world is mostly open sky: a cell with no cover costs nothing and answers
/// "open" without being stored. The cell size is a declaration — metres, typically a few — and the
/// structure is rebuilt when the geometry it was derived from changes, not per frame.
class SkyOcclusion {
public:
    explicit SkyOcclusion(Allocator& allocator) noexcept;

    SkyOcclusion(const SkyOcclusion&) = delete;
    SkyOcclusion& operator=(const SkyOcclusion&) = delete;

    [[nodiscard]] Status configure(f32 cell_metres) noexcept;

    /// Record that a rectangle of world is covered at `height`, hiding `occlusion` of the sky.
    ///
    /// This is what a cooker calls once per roof, bridge, arch and canopy. It is a rectangle and
    /// not a mesh, deliberately: an occlusion representation that needed the geometry would be a
    /// second scene, and the requirement asks for a COARSE structure derived from one.
    [[nodiscard]] Status add_cover(f64 min_x, f64 min_z, f64 max_x, f64 max_z, f32 height,
                                   f32 occlusion) noexcept;

    void clear() noexcept;

    [[nodiscard]] ShelterSample shelter_at(const world::WorldVec3d& at) const noexcept;

    [[nodiscard]] usize cells() const noexcept { return cells_.size(); }
    [[nodiscard]] f32 cell_metres() const noexcept { return cell_metres_; }
    /// Lookups since construction. The cost, counted; see the header note.
    [[nodiscard]] u64 queries() const noexcept { return queries_; }
    void reset_queries() const noexcept { queries_ = 0; }

private:
    Allocator* allocator_;
    HashMap<u64, SkyCoverCell> cells_;
    f32 cell_metres_ = 4.0F;
    mutable u64 queries_ = 0;
};

// ================================================================================================
// PRESENTATION TIERS
// ================================================================================================

/// How precipitation is drawn at a distance band. The specification's three, named.
enum class PrecipitationTier : u8 {
    /// Effects near the camera.
    Particles = 0,
    /// A screen-space or volumetric approximation at middle distance.
    Approximation,
    /// Field state alone.
    FieldOnly,
    kCount,
};

inline constexpr u32 kPrecipitationTierCount = static_cast<u32>(PrecipitationTier::kCount);

[[nodiscard]] const char* precipitation_tier_name(PrecipitationTier tier) noexcept;

/// The levers a budget arbiter moves. Every one of them is presentation; none is read by anything
/// in cells.h, storm.h or accumulation.h, which is what makes "reducing them SHALL NOT alter
/// authoritative weather state" true by construction rather than by discipline.
struct PrecipitationLevers {
    /// The hard cap on simulated particles, whatever the rate. THIS IS THE "rain scales"
    /// requirement: raising the rate raises the apparent density through the approximation tiers,
    /// never the particle count past this number.
    u32 max_particles = 4096;
    /// Metres. Where the particle tier ends and the approximation begins.
    f32 particle_distance_metres = 60.0F;
    /// Metres. Where the approximation ends and field state alone carries it.
    f32 approximation_distance_metres = 400.0F;
    /// Multiplier on the particle count, [0, 1]. What a GPU budget arbiter actually moves.
    f32 density_scale = 1.0F;
};

/// What presentation is asked to draw. Produced from the state and the levers; read by nothing in
/// this module.
struct PrecipitationPlan {
    /// Particles per tier. Only `Particles` is ever non-zero — the other two draw no particles at
    /// all, which is what the tiering means — and it is carried per tier so a diagnostic can show
    /// the zeroes rather than leave them implied.
    u32 particles[kPrecipitationTierCount] = {};
    /// The band each tier covers, metres from the camera.
    f32 tier_start_metres[kPrecipitationTierCount] = {};
    f32 tier_end_metres[kPrecipitationTierCount] = {};
    /// Apparent density the approximation tier should render at, [0, 1]. Derived from the rate, and
    /// this is what carries a downpour past the particle cap.
    f32 approximation_density = 0.0F;
    /// The rate the plan was built from, and its type. Carried so a diagnostic does not have to
    /// re-derive them.
    f32 rate_mm_per_hour = 0.0F;
    PrecipitationType type = PrecipitationType::None;
    /// How far the wind blows a falling particle sideways per metre of fall. The one number the
    /// presentation needs from the wind, computed here so every tier uses the same slant.
    Vec2 slant{0.0F, 0.0F};
};

/// Build the plan. A free function over (state, wind, levers): it holds nothing, so a renderer can
/// call it per view without a presentation object existing at all.
[[nodiscard]] PrecipitationPlan plan_precipitation(const WeatherState& state, const Vec2& wind,
                                                   const PrecipitationLevers& levers) noexcept;

/// What an interaction effect — a splash, a drip, a surface impact — is driven by.
///
/// "Interaction effects SHALL be driven by that occlusion and by the wetness field rather than by
/// particle collision events", so this takes the shelter sample and the wetness and returns rates.
/// There is no collision event in the signature, and no particle either.
struct InteractionRates {
    /// Splashes per square metre per second on exposed ground.
    f32 splash_rate = 0.0F;
    /// Drips per square metre per second from the edge of cover.
    f32 drip_rate = 0.0F;
    /// [0, 1]: how strongly a surface should read as being struck.
    f32 impact_strength = 0.0F;
};

[[nodiscard]] InteractionRates interaction_rates(const WeatherState& state,
                                                 const ShelterSample& shelter,
                                                 f32 wetness) noexcept;

}  // namespace cy::weather
