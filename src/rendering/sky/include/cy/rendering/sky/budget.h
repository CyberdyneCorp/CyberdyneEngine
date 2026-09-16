#pragma once
// THE SKY'S SUBSYSTEM CONTROLLER: the first thing under `src/` that reports a MEASURED cost to the
// renderer's budget arbiter. M11.c task 5.3.
//
// `rendering-architecture` — "Subsystem controllers SHALL report their measured cost to the
// arbiter", and the requirement M11.c added under it:
//
//   "A subsystem controller SHALL report a cost derived from an OBSERVATION of its own work in a
//   frame — a timer, a counter scaled by a measured unit cost, or a device query — and SHALL NOT
//   report a value that is a compile-time constant or a table lookup independent of the frame."
//
// ================================================================================================
// WHAT WAS ALREADY TRUE, AND WHY IT WAS NOT ENOUGH
// ================================================================================================
//
// M10 gave this module `sky_quality_ladder()`: the sky's five tiers PRICED in the arbiter's own
// currency, declared to it through `profile.h`. That is half of "tier selection SHALL follow the
// renderer profile and the budget arbiter" and it is the half that can be written without running
// anything. The other half is a controller that tells the arbiter what the sky actually cost, and
// until this file there was no instance of `rendering::SubsystemController` anywhere under `src/`:
// the seven that existed live in `samples/07-fidelity` over a hard-coded table.
//
// An arbiter fed declarations converges beautifully on a model of the frame rather than on the
// frame. That is not a figure of speech — the spike that settled every constant in `ArbiterConfig`
// WAS a model, it is `samples/07-fidelity`, and it is exactly as stable today as it was in M7.
//
// ================================================================================================
// A COUNTER SCALED BY A MEASURED UNIT COST, AND WHY NOT THE RAW TIMER
// ================================================================================================
//
// The requirement names three admissible observations and this takes the middle one. `observe()` is
// given what the sky DID — density samples the cloud march took, density samples the shadow field
// took, sky-view rows re-integrated — and how long it took to do it, and it keeps the RATIO: a
// filtered nanoseconds-per-sample learned from the clock. The cost it reports is that unit cost
// times THIS frame's count.
//
// Both halves are needed and neither alone would do:
//
//   * The raw elapsed time alone is the machine's answer to a question about the sky. It carries
//     every preemption, every other subsystem that shared the core, and the noise of a clock read
//     twice; the arbiter already filters the FRAME with an EMA for that reason, and a second noisy
//     signal underneath it is what `design.md` §2.7 says to lower `filter_alpha` for rather than to
//     widen the margins against.
//   * The count alone is not a cost at all. Samples are not milliseconds and the conversion is a
//     property of the machine, which is the one thing a declared price cannot know — see
//     `sky_quality_ladder()`'s own note that its prices "are the engine's defaults and not
//     measurements of any particular machine".
//
// The unit cost is learned from the clock, so nothing here is a constant; the count is this
// frame's, so the reported cost moves when the sky's work moves. THAT MOVEMENT IS WHAT MAKES IT A
// MEASUREMENT AND `rendering::CostSource` IS WHERE IT IS JUDGED: a controller that reported the
// same number every frame would be reported `Estimated` by the arbiter however it arrived there.
//
// ================================================================================================
// WHICH ALLOCATION THE SKY SPENDS, SINCE IT IS NOT ONE OF THE SEVEN
// ================================================================================================
//
// `profile.h` says it and this file obeys it: `rendering-architecture` names the arbiter's
// subsystems and the sky is not among them, so the sky declares a ladder in the arbiter's currency
// and the renderer folds it into an allocation. The engine folds it into
// `BudgetSubsystem::PostProcessing` because the sky's expensive half is the volumetric cloud march,
// a screen-space integration through the froxel volume `rendering-post-processing` owns and that
// `src/rendering/sky/CMakeLists.txt` already records this module as sharing rather than
// duplicating. A project that draws its clouds somewhere else passes a different subsystem;
// `declare()` takes it as an argument for that reason and not as a matter of taste.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/arbiter/arbiter.h>
#include <cy/rendering/arbiter/subsystem.h>
#include <cy/rendering/sky/profile.h>

namespace cy::rendering::sky {

/// The allocation the engine folds the sky into. See the header for the argument.
inline constexpr BudgetSubsystem kSkyBudgetSubsystem = BudgetSubsystem::PostProcessing;

/// What the sky DID in one frame, counted by the code that did it.
///
/// Every member is a count something already keeps: `CloudMarchStats::density_samples` and
/// `::light_samples` summed over the rays a frame marched, `CloudShadowStats::density_samples`, and
/// the rows `IncrementalSkyView` re-integrated. Nothing here is authored and nothing is a tier
/// lookup — a lower tier shows up as FEWER SAMPLES rather than as a smaller number somebody wrote
/// down, which is the whole difference between this and the cost table in `samples/07-fidelity`.
struct SkyWorkload {
    u64 cloud_density_samples = 0;
    u64 cloud_light_samples = 0;
    u64 shadow_density_samples = 0;
    u64 sky_view_rows = 0;

    /// The one number the unit cost is per. Summed rather than weighted: a weighting would be a
    /// declared price for one kind of sample against another, which is the thing this class exists
    /// not to contain. A sky-view row is charged as the table's own width in samples.
    [[nodiscard]] u64 samples() const noexcept;
};

/// What the controller observed, for a diagnostic and for a test to read.
struct SkyBudgetStats {
    u32 observations = 0;
    /// Frames whose observation could not price anything — no work, or no elapsed time to divide
    /// by. Counted rather than silently skipped, because "the sky reported nothing this frame" and
    /// "the sky was free this frame" must not look the same.
    u32 unpriced = 0;
    u64 last_samples = 0;
    /// The learned conversion, in nanoseconds per sample. The one number here that is a property of
    /// the machine rather than of the frame.
    f32 unit_cost_ns = 0.0F;
    f32 last_elapsed_ms = 0.0F;
    f32 reported_ms = 0.0F;
};

/// The sky's controller.
///
/// It holds a `rendering::SubsystemController` — the engine's one control loop, not a second one —
/// and adds the two things that loop cannot supply for itself: an observation of the sky's own
/// work, and the map from a ladder position to a `SkyQualityTier`.
class SkyBudget {
public:
    SkyBudget() noexcept = default;

    /// Declare the sky's priced ladder to the arbiter and to the controller behind it.
    ///
    /// `base_cost_ms` is the estimate the loop runs on until the first observation arrives, and it
    /// is the ONLY declared cost in this class. It is a compile-time default, it is reported as
    /// `Estimated` for exactly as long as it is the answer, and `observe()` replaces it.
    [[nodiscard]] Status declare(BudgetArbiter& arbiter,
                                 BudgetSubsystem subsystem = kSkyBudgetSubsystem,
                                 u32 reduction_order = 4) noexcept;

    /// Start the clock on the sky's own work.
    void begin_frame() noexcept;

    /// Stop it, and price what the frame did. `begin_frame()` must have run.
    void end_frame(const SkyWorkload& work) noexcept;

    /// The same, for a caller that already has the elapsed time — a device timestamp query, a job
    /// system's own accounting, or a test that must not depend on a wall clock.
    void observe(const SkyWorkload& work, u64 elapsed_nanoseconds) noexcept;

    /// Report to the arbiter. Separate from `observe()` because a frame observes once and reports
    /// once, and because the arbiter is not this class's to hold.
    void submit(BudgetArbiter& arbiter) const noexcept;

    /// Take the arbiter's answer: the allocation, the relax grant, and the pinned state.
    ///
    /// PINNING COMES THROUGH HERE AND NOWHERE ELSE, which is what makes "a pinned mode SHALL
    /// disable the arbiter and every subsystem controller together ... partial pinning SHALL NOT be
    /// possible" true of the sky by construction rather than by a caller remembering.
    [[nodiscard]] SubsystemUpdate apply(const ArbiterReport& report) noexcept;

    /// The cost this controller reports, in milliseconds.
    [[nodiscard]] f32 cost_ms() const noexcept { return stats_.reported_ms; }
    /// What that cost is: the claim, confirmed against the numbers.
    [[nodiscard]] CostSource cost_source() const noexcept { return controller_.cost_source(); }

    /// Back to authored quality: what a release from pressure, or a profile change, does. The
    /// engine's `BudgetArbiter::restore_authored_quality()` moves the allocations; this moves the
    /// sky's own position, because the controller is the only thing that knows where it stands.
    void restore_authored_quality() noexcept { controller_.restore_authored_quality(); }

    /// The tier the ladder position selects, and the settings that draw it.
    [[nodiscard]] SkyQualityTier tier() const noexcept;
    [[nodiscard]] SkyQualitySettings settings() const noexcept { return sky_quality(tier()); }

    [[nodiscard]] const SubsystemController& controller() const noexcept { return controller_; }
    [[nodiscard]] const SkyBudgetStats& stats() const noexcept { return stats_; }
    [[nodiscard]] BudgetSubsystem subsystem() const noexcept { return subsystem_; }
    [[nodiscard]] bool declared() const noexcept { return declared_; }

private:
    SubsystemController controller_;
    SkyBudgetStats stats_;
    BudgetSubsystem subsystem_ = kSkyBudgetSubsystem;
    u64 began_nanoseconds_ = 0;
    bool timing_ = false;
    bool declared_ = false;
};

}  // namespace cy::rendering::sky
