#pragma once
// IMPORTANCE CLASSES AND THE FRAME-BUDGET CONTROLLER. M8.c task 2.6.
//
// ================================================================================================
// THE CONTROLLER CANNOT SEE THE FRAME, AND THAT IS THE POINT
// ================================================================================================
//
// `vfx-system`: "The controller SHALL measure VFX cost and report it to the arbiter. It SHALL NOT
// measure total frame time or infer global load, because a cost VFX did not incur is not a cost VFX
// should correct for."
//
// So `measure()` takes ONE number and it is VFX's own milliseconds. There is no frame time in this
// interface and nowhere to put one — which makes the requirement structural rather than a rule
// somebody has to remember. `vfx-system`'s "VFX does not pay for another subsystem's cost" scenario
// is then true by construction, and `test_vfx_runtime.cpp` still checks it, because a structural
// claim that nothing exercises is a claim about a header.
//
// ================================================================================================
// ADJUSTMENT ORDER, THE RESERVED MINIMUM, AND HYSTERESIS
// ================================================================================================
//
// "Adjustment SHALL proceed from least to most important: `Decorative` first, `Critical` last.
// `Critical` effects SHALL have reserved capacity and SHALL be degraded only when no other headroom
// remains." `ImportanceClass`'s enumerator order IS the rank, so the loop that reduces runs from
// the highest enumerator down and the loop that restores runs the other way.
//
// "The controller SHALL declare a reserved minimum, and SHALL report when it has reached it so the
// arbiter can reallocate rather than continue reducing a subsystem with nothing left to give." That
// is `BudgetState::at_reserved_minimum`, and it becomes true when every class is at its floor —
// not when the controller feels it has done enough.
//
// "Adjustments SHALL be applied smoothly and hysteretically so quality does not visibly oscillate,
// and SHALL operate on a SHORTER TIME CONSTANT than the arbiter's reallocation." Two thresholds
// rather than one, and a rate limit per second: quality cannot move faster than `kAdjustRate`, and
// a measurement between the two thresholds moves nothing at all.
//
// ================================================================================================
// PINNED IS GLOBAL AND MEANS "REPORT, DO NOT CORRECT"
// ================================================================================================
//
// "Pinned mode SHALL be global: when the arbiter is pinned, this controller SHALL be pinned with
// it", and "WHEN pinned mode is enabled THEN no adaptive adjustment SHALL occur, and exceeding the
// budget SHALL be REPORTED rather than corrected." `set_pinned(true)` freezes every lever and
// `BudgetState::over_budget` is how the excess is reported.

#include <cy/core/base/types.h>
#include <cy/vfx/asset.h>

namespace cy::vfx {

/// What the controller has decided for one importance class. Every member is a lever `vfx-system`
/// names by hand: "spawn rates, simulation frequency, particle count caps, collision quality,
/// renderer feature level (lighting, shadows, sorting), and effect LOD".
struct BudgetLevers {
    /// Multiplier on the authored spawn rate.
    f32 spawn_scale = 1.0F;
    /// The simulation frequency this class runs at, in hertz.
    f32 simulation_hz = 60.0F;
    /// Multiplier on the authored particle count cap.
    f32 count_cap_scale = 1.0F;
    /// 3 is the authored quality; 0 is off.
    u8 collision_quality = 3;
    /// 3 per-pixel lit with shadows; 2 per-pixel; 1 vertex lit; 0 unlit.
    u8 feature_level = 3;
    /// Added to the effect's LOD index.
    u8 lod_bias = 0;
    /// False drops distance sorting to an approximation.
    bool sorted = true;
};

/// Which levers the controller actually moved on the last update, so "which levers did it apply"
/// is a set rather than a sentence.
struct AppliedAdjustments {
    bool spawn_rate = false;
    bool simulation_frequency = false;
    bool count_cap = false;
    bool collision = false;
    bool feature_level = false;
    bool lod = false;
    bool sorting = false;

    [[nodiscard]] u32 count() const noexcept;
};

struct BudgetState {
    /// The allocation the renderer's budget arbiter issued, in milliseconds.
    f32 allocation_ms = 2.0F;
    /// VFX's own measured cost. Never a frame time — see the note at the top of this file.
    f32 measured_ms = 0.0F;
    /// The cost the controller expects to reach if it can degrade no further.
    f32 reserved_minimum_ms = 0.4F;
    /// Every class is at its floor: the controller has nothing left to give.
    bool at_reserved_minimum = false;
    /// Measured above the allocation. In pinned mode this is the whole of the response.
    bool over_budget = false;
    bool pinned = false;
    /// The least important class that is not at its authored quality, or `Count` when nothing is
    /// reduced. Reading it top-down answers "how far down the ranks has this gone".
    ImportanceClass lowest_reduced = ImportanceClass::Count;
    /// Quality in [0, 1] per class: 1 is authored, 0 is the class's floor.
    f32 quality[kImportanceCount] = {1.0F, 1.0F, 1.0F, 1.0F};
};

/// The reduction threshold, as a multiple of the allocation. Below `kRestoreThreshold` quality is
/// given back; between the two nothing moves, which is the hysteresis.
inline constexpr f32 kReduceThreshold = 1.05F;
inline constexpr f32 kRestoreThreshold = 0.85F;
/// The most quality may change in one second. The arbiter's own reallocation is slower, which is
/// the ordering `vfx-system` requires.
inline constexpr f32 kAdjustRate = 2.0F;
/// `Critical` never goes below this quality. "Critical effects SHALL still render at their
/// configured minimum quality."
inline constexpr f32 kCriticalFloor = 0.5F;

class BudgetController {
public:
    BudgetController() noexcept;

    void set_allocation(f32 milliseconds) noexcept;
    void set_reserved_minimum(f32 milliseconds) noexcept;
    /// Global, and set from the arbiter. See the note at the top of this file.
    void set_pinned(bool pinned) noexcept;

    /// VFX's own cost for the last frame, in milliseconds. THE ONLY MEASUREMENT THIS CLASS TAKES.
    void measure(f32 vfx_milliseconds) noexcept;

    /// Advance the controller by `dt` seconds. Smooth and hysteretic; nothing moves faster than
    /// `kAdjustRate` and nothing moves at all between the two thresholds.
    void update(f32 dt) noexcept;

    [[nodiscard]] const BudgetLevers& levers(ImportanceClass importance) const noexcept;
    [[nodiscard]] const BudgetState& state() const noexcept { return state_; }
    [[nodiscard]] const AppliedAdjustments& applied() const noexcept { return applied_; }

    /// Apply an effect's own floors to the levers this controller computed. The controller holds
    /// the class's policy; the ASSET holds the effect's, and neither may be ignored.
    [[nodiscard]] BudgetLevers levers_for(ImportanceClass importance,
                                          const ScalabilityPolicy& policy) const noexcept;

    /// Put every class back at its authored quality. What a capture, a load or a level change does.
    void reset() noexcept;

private:
    void recompute_levers() noexcept;

    BudgetState state_;
    BudgetLevers levers_[kImportanceCount];
    AppliedAdjustments applied_;
};

}  // namespace cy::vfx
