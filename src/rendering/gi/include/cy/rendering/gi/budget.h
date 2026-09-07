#pragma once
// The GI allocation, and the declared levers it is spent through. Task 9.2.
//
// `rendering-global-illumination` — "GI budget and importance".
//
// ================================================================================================
// THIS SUBSYSTEM CANNOT SEE THE FRAME, AND THAT IS ENFORCED BY THE SIGNATURE
// ================================================================================================
//
// `rendering-architecture` forbids a subsystem controller to measure total frame time, and the
// M7 arbiter spike (design.md §2.6) found the unwritten second half: a controller must not RELAX on
// its own authority either, because the time a step back up costs comes out of the frame it cannot
// see. Both halves are structural here:
//
//   * `update()` takes `measured_gi_ms` and nothing else. There is no parameter a frame time could
//     be passed in through, so a call site cannot get this wrong by accident.
//   * relaxing requires `permit_relaxation()` to have been called by the arbiter this tick. Without
//     it the loop only ever tightens, which is the correct behaviour for a controller that has been
//     given an allocation and no permission to exceed it.
//
// ================================================================================================
// THE ACTUATOR IS THE DECLARED REDUCTION ORDER, NEVER A UNIFORM SCALE
// ================================================================================================
//
// design.md §2.3, and it is the spike's single most important finding: a uniform scale over every
// lever moves whichever ones happen to sit nearest a ladder boundary, so one tick drops four at
// once and the next gives them all back. Replacing it with "walk ascending `reduction_order` and
// force ONE step per lever until the gain covers the deficit" took the spike's sweep from 27 of 71
// loads oscillating to 2, before the remaining fixes took it to zero.
//
// The same shape at a shorter time constant is what this class is, which is what
// "Adjustment SHALL be smooth and hysteretic, on a shorter time constant than the arbiter's
// reallocation" means.
//
// ================================================================================================
// A LEVER DECLARES WHAT EACH POSITION COSTS — THE ONE THING §2.10 ASKED FOR
// ================================================================================================
//
// `GiLeverSchedule::relative_cost` is the fourth field the arbiter spike found `residency`'s lever
// shape needed: an arbiter allocating milliseconds over a ladder it cannot price is choosing blind,
// and every mechanism above is expressed in terms of that number.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::rendering::gi {

/// The quality axes the GI allocation is distributed across. The specification's own list.
enum class GiLever : u8 {
    ProbeUpdates = 0,
    TracedRays,
    TracingResolution,
    SurfaceCacheRate,
    ProbeDensity,
    ReflectionResolution,
    DenoiserQuality,
    Count,
};

inline constexpr u32 kGiLeverCount = static_cast<u32>(GiLever::Count);
inline constexpr u32 kMaxLeverPositions = 6;

[[nodiscard]] const char* gi_lever_name(GiLever lever) noexcept;

/// One lever's declared ladder. The same shape `residency::LeverSchedule` has, plus the cost field.
struct GiLeverSchedule {
    /// Positions, coarsest last. Position 0 is authored quality.
    u32 position_count = 1;
    /// The value at each position — probes per frame, rays per probe, a resolution scale. What the
    /// consuming subsystem actually reads.
    f32 value[kMaxLeverPositions] = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    /// What each position costs relative to position 0. Position 0 is 1.0 by construction.
    f32 relative_cost[kMaxLeverPositions] = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    /// Lower reduces first. `residency`'s meaning, unchanged.
    u32 reduction_order = 0;
    /// False for a lever this project does not have. A lever nobody declared is not pretended to
    /// exist, which is the difference between "no reduction available" and "reduced to nothing".
    bool declared = false;
};

/// The default ladders. Real numbers rather than placeholders: they are what `system.h` runs with
/// and what the tests drive.
[[nodiscard]] GiLeverSchedule default_schedule(GiLever lever) noexcept;

struct GiBudgetSettings {
    /// What the renderer budget arbiter allocated to illumination, in milliseconds.
    f32 allocation_ms = 3.0F;
    /// Below this the system reports it has reached its reserved minimum and stops reducing.
    f32 reserved_minimum_ms = 0.6F;
    /// Tighten when the filtered cost exceeds the allocation by this factor. design.md §2.7: the
    /// margin must exceed the noise that survives the filter, and 6% is twice the +/-3% the spike
    /// modelled.
    f32 tighten_margin = 1.06F;
    /// Relax when the predicted cost of the next better position is under this factor. The two
    /// margins straddle the allocation, which is what stops the ratchet.
    f32 relax_margin = 0.88F;
    /// Frames a relaxation waits after the last lever change. Relaxing answers a frame that is
    /// fine, and there is no hurry.
    u32 relax_dwell_frames = 6;
    /// The exponential filter on the measured cost. design.md §2.2.
    f32 filter_alpha = 0.25F;
    /// Fraction of the deficit one tightening tick covers.
    f32 gain = 0.35F;
};

struct GiBudgetReport {
    f32 measured_ms = 0.0F;
    f32 filtered_ms = 0.0F;
    f32 predicted_ms = 0.0F;
    f32 allocation_ms = 0.0F;
    u32 levers_tightened = 0;
    u32 levers_relaxed = 0;
    /// The lever that moved last, for the "reporting each lever applied" requirement.
    GiLever last_lever = GiLever::Count;
    bool at_reserved_minimum = false;
    bool pinned = false;
    /// True while the loop refused to relax because the arbiter had not permitted it. Not an
    /// error — it is the rule — but the number that explains a system sitting below its allocation.
    bool relaxation_withheld = false;
};

/// The GI allocation, held and distributed.
class GiBudget {
public:
    GiBudget() noexcept;

    void configure(const GiBudgetSettings& settings) noexcept;
    [[nodiscard]] const GiBudgetSettings& settings() const noexcept { return settings_; }

    /// Replace one lever's ladder. Refused when position 0's relative cost is not 1, because every
    /// other number in this class is relative to it.
    [[nodiscard]] Status declare(GiLever lever, const GiLeverSchedule& schedule) noexcept;
    [[nodiscard]] const GiLeverSchedule& schedule(GiLever lever) const noexcept;

    void set_allocation_ms(f32 allocation_ms) noexcept;

    /// Pinned mode is global with the arbiter: the loop stops, quality stays where it is, and an
    /// overrun is REPORTED rather than corrected.
    void set_pinned(bool pinned) noexcept { pinned_ = pinned; }
    [[nodiscard]] bool pinned() const noexcept { return pinned_; }

    /// The arbiter's permission to take a step back up, valid for one `update()`.
    void permit_relaxation() noexcept { relaxation_permitted_ = true; }

    /// Run the loop for one frame. `measured_gi_ms` is THIS SYSTEM'S OWN cost. See the header.
    GiBudgetReport update(f32 measured_gi_ms, u64 frame) noexcept;

    [[nodiscard]] u32 position(GiLever lever) const noexcept {
        return positions_[static_cast<u32>(lever)];
    }
    /// The value the consuming subsystem reads: probes per frame, rays per probe, a scale.
    [[nodiscard]] f32 value(GiLever lever) const noexcept;
    /// The whole system's predicted cost at the current positions, in milliseconds.
    [[nodiscard]] f32 predicted_cost_ms() const noexcept;
    [[nodiscard]] bool at_reserved_minimum() const noexcept;

    /// Restore every lever to position 0. What a release from a load spike does, and what a test
    /// does between cases.
    void reset_positions() noexcept;

    // --- Importance ------------------------------------------------------------------------------

    /// The quality multiplier one object gets, from what the view sees and what the game says.
    ///
    /// "Distribution SHALL be importance-aware, combining screen coverage with per-object
    /// importance from the ECS, so quality follows what matters in the game rather than distance
    /// alone." A gameplay-critical unit and background scenery at the same coverage do not get the
    /// same number, which is the whole point.
    [[nodiscard]] static f32 importance_weight(f32 screen_coverage, f32 object_importance) noexcept;

    /// A foveation mask, sampled in normalised viewport coordinates. Empty means none, and sample
    /// density then follows importance alone.
    [[nodiscard]] Status set_foveation_mask(Span<const f32> mask, u32 width, u32 height) noexcept;
    [[nodiscard]] f32 foveation_at(f32 u, f32 v) const noexcept;
    [[nodiscard]] bool has_foveation_mask() const noexcept { return foveation_width_ != 0; }

private:
    void tighten_to_cover(f32 deficit, u64 frame, GiBudgetReport& report) noexcept;
    void try_relax(u64 frame, GiBudgetReport& report) noexcept;
    [[nodiscard]] bool any_lever_reduced() const noexcept;
    [[nodiscard]] bool tighten_one(GiBudgetReport& report) noexcept;
    [[nodiscard]] bool relax_one(GiBudgetReport& report) noexcept;
    [[nodiscard]] f32 cost_at(GiLever lever, u32 position) const noexcept;

    GiBudgetSettings settings_{};
    GiLeverSchedule schedules_[kGiLeverCount];
    u32 positions_[kGiLeverCount] = {};
    f32 filtered_ms_ = 0.0F;
    bool filter_primed_ = false;
    bool pinned_ = false;
    bool relaxation_permitted_ = false;
    u64 last_change_frame_ = 0;
    bool changed_once_ = false;

    Array<f32> foveation_;
    u32 foveation_width_ = 0;
    u32 foveation_height_ = 0;
};

}  // namespace cy::rendering::gi
