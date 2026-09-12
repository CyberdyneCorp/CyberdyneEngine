#pragma once
// The water query: one answer, for any position, from any consumer. M10 task 2.3.
//
// `water` — "Water queries": "The engine SHALL provide a water query returning, for a world
// position: surface height, surface normal, velocity, depth to bed, the water column thickness,
// density, and the identity of the body. Queries SHALL return the AUTHORITATIVE displacement bands,
// batchable for many positions, and SHALL NOT block or require a physics query. A query in a region
// whose water data is not resident SHALL return the body's mean level with a resolution indicator."
//
// Every member of that sentence is a member of `WaterSample` below, and the two properties that are
// not values are properties of the code that fills it: `WaterSystem::query()` takes no lock, starts
// no job and calls into no physics world — it evaluates the body's own backend and returns.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/water/body.h>

namespace cy::water {

/// How the answer was arrived at — the specification's "resolution indicator".
///
/// It is not a precision in metres because the question a consumer asks is not "how precise" but
/// "is this the simulation or the fallback": a hull that is being floated on a mean level is not
/// slightly wrong, it is on the wrong surface, and a number would invite a tolerance where a branch
/// belongs.
enum class WaterResolution : u8 {
    /// No body owns this position.
    None = 0,
    /// The body's mean level, because its data is not resident here. The specification's fallback.
    MeanLevel,
    /// The body's backend was evaluated at this position.
    Simulated,
};

[[nodiscard]] const char* water_resolution_name(WaterResolution resolution) noexcept;

/// Where a character is, relative to the water. `water` — "Characters SHALL support swimming and
/// wading states derived from WATER DEPTH at their position."
enum class CharacterWaterState : u8 { Dry = 0, Wading, Swimming };

[[nodiscard]] const char* character_water_state_name(CharacterWaterState state) noexcept;

/// One answer. Every member of the specification's list, and nothing derived that a consumer could
/// not compute from the rest.
struct WaterSample {
    /// The body that owns the position, or an invalid identity where none does.
    WaterBodyId body;
    WaterResolution resolution = WaterResolution::None;

    /// Absolute surface height, metres. `mean_level` where the resolution says so.
    f64 surface_height = 0.0;
    Vec3 normal{0.0F, 1.0F, 0.0F};
    /// The water's velocity at the position, m/s: the surface's own orbital velocity for an open
    /// water body, the flow for a river, and their sum where a river is inside an ocean's bounds.
    Vec3 velocity{0.0F, 0.0F, 0.0F};

    /// Distance from the sampled position DOWN to the bed, metres. Negative when the position is
    /// below the bed — inside the ground — which a caller clamping to zero would not be able to
    /// tell from standing exactly on it.
    f32 depth_to_bed = 0.0F;
    /// The thickness of the water column at this position: surface minus bed, never negative.
    /// What absorption integrates over and what a boat's draught is compared against.
    f32 column_thickness = 0.0F;
    /// How deep the sampled position is below the surface, metres. Negative above the surface, so
    /// a hull point in the air and one just under it differ by a sign rather than by a flag.
    f32 submersion = 0.0F;

    f32 density = 0.0F;
    /// The breaking indicator at the surface above the position, in [0, 1]. Foam's generator.
    f32 breaking = 0.0F;

    [[nodiscard]] bool in_water() const noexcept { return submersion > 0.0F; }
};

/// The depth thresholds a character's state is derived from. Declared rather than constants,
/// because a knee is a different height on a dwarf and a wading depth is a design decision.
struct SwimThresholds {
    f32 wade_metres = 0.4F;
    f32 swim_metres = 1.4F;
};

[[nodiscard]] CharacterWaterState character_state(f32 water_depth,
                                                  const SwimThresholds& thresholds) noexcept;

/// What the diagnostics requirement calls "for a position, which body owns it, what the
/// authoritative surface height is, and which bands contributed".
struct WaterPointReport {
    WaterSample sample;
    const char* body_name = "";
    WaterBodyType type = WaterBodyType::Lake;
    WaterBackend backend = WaterBackend::Flat;
    /// Filled from `band_contributions()`; `band_count` says how many are valid.
    u32 band_count = 0;
    f32 authoritative_metres = 0.0F;
    f32 visual_metres = 0.0F;
    /// Every body containing the position, in the declared resolution order — so an overlap is
    /// visible in the report rather than only in its result.
    u32 overlapping_bodies = 0;
};

}  // namespace cy::water
