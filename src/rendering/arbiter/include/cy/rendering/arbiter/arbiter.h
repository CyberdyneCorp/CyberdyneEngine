#pragma once
// The renderer budget arbiter: exactly one of these, and the only thing in the renderer that is
// given the frame time. Task 10.1.
//
// `rendering-architecture` — "Renderer budget arbiter". M7 `design.md` §2 is the spike that settled
// every constant in `ArbiterConfig`, and §2.2 is the table they are read from.
//
// ================================================================================================
// THE ACTUATOR IS THE DECLARED REDUCTION ORDER, NEVER A UNIFORM SCALE
// ================================================================================================
//
// This is `design.md` §2.3, and it is the single most important finding of the spike. When the
// frame is over budget by `e`, the arbiter walks the subsystems in ascending `reduction_order` and
// forces ONE step down from each — by setting that subsystem's allocation just above what the NEXT
// position costs — until `gain * e` of the deficit is covered. Nothing is scaled.
//
// A uniform scale over every allocation moves whichever subsystems happen to sit nearest a ladder
// boundary, so one tick drops four of them at once and the next tick gives them all back.
// Replacing the uniform scale with the declared order took the spike's sweep from 27 of 71 loads
// oscillating (worst 411 lever changes) to 2 of 71 (worst 10).
//
// ================================================================================================
// FOUR MECHANISMS, AND NO SINGLE ONE IS SUFFICIENT
// ================================================================================================
//
// `design.md` §2.4 is a factorial over them: an EMA filter, a relax dwell, an arbiter period of 8
// frames, and a deadband. Each alone leaves 56 to 64 of 71 loads oscillating; all four together
// leave zero. The deadband is the one that finishes the job and it is sized against the COARSEST
// SINGLE-LEVER COST QUANTUM REACHABLE FROM THE CURRENT STATE, not against a fixed number of
// milliseconds — because what makes a discrete ladder oscillate is a correction smaller than the
// step it would have to take.
//
// THE DEADBAND HAS A PRICE AND IT IS PAID DELIBERATELY. A deadband centred on the budget is by
// construction a refusal to correct an error smaller than one lever quantum, so the loop settles
// happily OVER the budget. `setpoint = budget - deadband` puts the band's upper edge on the budget
// instead. It costs 0.60 ms of a 12.70 ms allocatable budget in the spike's model and it is why
// zero of 71 loads settle over budget rather than two.
//
// ================================================================================================
// RESTORING QUALITY IS NOT THE INVERSE OF TAKING IT AWAY
// ================================================================================================
//
// `design.md` §2.5, four separate defects, all found in the spike's own drafts:
//
//   1. An allocation is CAPPED at what the subsystem can spend at position 0. Without the cap every
//      under-budget frame scales the allocations up again, nothing changes because everything is
//      already at position 0, and the next spike is absorbed by accumulated slack instead of by the
//      levers — a 54-frame limit cycle that looked like a control-law defect.
//   2. Headroom is MEASURED (`setpoint - filtered frame`), never book-kept as
//      `allocatable - sum of allocations`. The second is wrong by exactly the amount the ladder
//      wastes.
//   3. A grant is ALL OR NOTHING. A partial grant that cannot buy the step spends the surplus and
//      changes nothing, and the arbiter then creeps the level up until the step fires between two
//      ticks, at a moment nobody chose.
//   4. A relaxation must leave the frame at least one deadband inside the budget, which is what
//      stops the two branches handing the frame back and forth.
//
// ================================================================================================
// THE CRITERION IS CERTIFIED OVER A SWEEP, NEVER OVER ONE LOAD
// ================================================================================================
//
// The levers are a discrete ladder, so whether a control law oscillates depends on where its
// equilibrium happens to land relative to a ladder boundary. ONE STEP MAGNITUDE CAN MAKE ANY LAW
// LOOK STABLE — the spike's own first draft was certified at one load and oscillated on 27 of 71.
// `tests/test_sweep.cpp` is 71 step magnitudes, and it is the evidence for task 10.1 rather than
// any single run in `tests/test_arbiter.cpp`.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/arbiter/subsystem.h>

namespace cy::rendering {

/// The arbiter's tuning. Every value is `design.md` §2.2's, and the comment on each says what the
/// spike measured rather than what it seemed reasonable to pick.
struct ArbiterConfig {
    /// The frame budget, in milliseconds. 13.90 is 72 Hz with a margin, and is the spike's model.
    f32 frame_budget_ms = 13.90F;

    /// What the frame spends that the arbiter cannot allocate: present, the swap, the fixed cost of
    /// the graph itself. Subtracted from the budget before anything is allocated; a frame budget
    /// that pretended this was zero would over-allocate by exactly this much.
    f32 non_allocatable_ms = 1.20F;

    /// The arbiter's period. "The arbiter SHALL adjust allocations on a longer time constant than
    /// the subsystem controllers adjust within them" — the controllers run every frame.
    u32 period_frames = 8;

    /// Cover this fraction of the deficit per tick. Anything from 0.20 to 2.00 is stable at 1 to 8
    /// frames of readback latency (`design.md` §2.8), so this is not a number that has to be tuned
    /// against the readback depth.
    f32 gain = 0.35F;

    /// The deadband, as a multiple of the coarsest reachable single-step cost quantum. The sweep's
    /// edge is at 0.20; 0.5 is the recommended safety factor, and 1.0 costs 2.24 ms of a 12.70 ms
    /// budget for nothing the sweep can measure.
    f32 deadband_multiple = 0.5F;

    /// EMA alpha on the measured frame time. An engine whose GPU timings are noisier than +/-3%
    /// should LOWER this rather than widen the controllers' margins (`design.md` §2.7).
    f32 filter_alpha = 0.25F;

    /// A forced allocation is the target position's predicted cost times this. Same reason as the
    /// controllers' tighten margin: the margin must exceed the noise that survives the filter.
    f32 forced_allocation_margin = 1.08F;

    /// A GRANTED allocation is the target position's predicted cost times this, and it must be
    /// larger than the forced one. **This number is a defect found by this implementation's own
    /// restoration test and it is not in `design.md` §2.2.**
    ///
    /// The two halves of the loop have to agree, and at x1.08 they do not. A controller relaxes
    /// only when `predicted(next better) <= allocation * 0.88`, so the allocation that buys a step
    /// up must be at least `predicted / 0.88` = predicted x 1.136. An arbiter granting x1.08 spends
    /// its surplus on an allocation the controller then refuses to use — which is exactly
    /// `design.md` §2.5 defect 3, "a partial grant that cannot buy the step is the worst of both",
    /// arriving through a different door. Measured: 1 of 15 loads returned to authored quality at
    /// x1.08 and 15 of 15 at x1.20.
    ///
    /// 1.20 rather than 1.14 for the same safety factor the deadband takes: the arbiter's scale
    /// estimate and the controller's are two EMAs of the same signal at two update rates and they
    /// are not identical.
    f32 relax_allocation_margin = 1.20F;

    /// Frames before the arbiter will grant the same subsystem another step back up. Relaxing
    /// answers a frame that is fine; there is no hurry.
    u32 relax_grant_dwell_frames = 18;

    /// The resolution scale ladder — the LAST lever, reached only when every registered subsystem
    /// reports it is at its minimum. Discrete rather than continuous for the same reason every
    /// other lever is: a continuous scale creeps, and a creep is a change nobody chose.
    f32 resolution_scale[kMaxLadderPositions] = {1.00F, 0.92F, 0.85F, 0.78F, 0.71F, 0.65F};
    u8 resolution_positions = 6;
};

/// Why an allocation moved. "Adjustments are attributable" — the report states which allocation
/// changed, by how much, and what measurement caused it.
enum class AdjustmentCause : u8 {
    /// The filtered frame time is above the setpoint by more than the deadband.
    FrameOverBudget = 0,
    /// The filtered frame time is below the setpoint by more than the deadband and by more than the
    /// step's predicted increase.
    HeadroomAvailable,
    /// Every registered subsystem reports it is at its minimum and the frame is still over budget.
    /// The only cause that reaches resolution scale.
    EverySubsystemAtMinimum,
    Count,
};

[[nodiscard]] const char* adjustment_cause_name(AdjustmentCause cause) noexcept;

/// One line of the report. `subsystem == BudgetSubsystem::Count` means resolution scale, which is
/// the only adjustment that is not a subsystem's.
struct BudgetAdjustment {
    BudgetSubsystem subsystem = BudgetSubsystem::Count;
    AdjustmentCause cause = AdjustmentCause::FrameOverBudget;
    u8 from_position = 0;
    u8 to_position = 0;
    f32 from_ms = 0.0F;
    f32 to_ms = 0.0F;
};

/// At most one step per subsystem per tick, plus one of resolution scale.
inline constexpr u32 kMaxAdjustmentsPerTick = kBudgetSubsystemCount + 1U;

/// What the arbiter reports every frame. `rendering-architecture`: "The arbiter SHALL report, per
/// frame: each allocation, each subsystem's measured cost, which subsystems are at their minimum,
/// and every adjustment made with its cause."
struct ArbiterReport {
    /// True on the frames the arbiter actually arbitrated — one frame in `period_frames`.
    bool arbitrated = false;
    bool pinned = false;

    f32 measured_frame_ms = 0.0F;
    f32 filtered_frame_ms = 0.0F;
    /// The budget minus the deadband. A settled frame sits at or below it.
    f32 setpoint_ms = 0.0F;
    f32 deadband_ms = 0.0F;
    /// Milliseconds over `frame_budget_ms` while pinned. Reported, never corrected.
    f32 pinned_overrun_ms = 0.0F;

    f32 resolution_scale = 1.0F;

    f32 allocation_ms[kBudgetSubsystemCount] = {};
    f32 measured_ms[kBudgetSubsystemCount] = {};
    bool registered[kBudgetSubsystemCount] = {};
    bool at_minimum[kBudgetSubsystemCount] = {};
    /// The arbiter permits this subsystem one step back up this frame. The controller consumes it.
    bool relax_granted[kBudgetSubsystemCount] = {};

    BudgetAdjustment adjustments[kMaxAdjustmentsPerTick] = {};
    u32 adjustment_count = 0;
};

/// The renderer's one budget arbiter.
///
/// Not thread-safe and not internally threaded: it is stepped once per frame from the thread that
/// owns the frame. It allocates nothing after `declare()`.
///
/// The loop, per frame:
///
///     arbiter.report_frame_ms(gpu_frame_time);            // only the arbiter gets this
///     for each subsystem: arbiter.report_subsystem(...);  // its own cost and where it stands
///     const ArbiterReport report = arbiter.update();
///     for each subsystem: controller.set_allocation_ms(report.allocation_ms[i]);
///                         if (report.relax_granted[i]) controller.grant_relax_step();
class BudgetArbiter {
public:
    BudgetArbiter() noexcept;

    /// Configure. Refuses a budget whose non-allocatable part is not smaller than the whole, a
    /// non-positive period, and a resolution ladder that is not descending from 1.0.
    Status configure(const ArbiterConfig& config) noexcept;

    [[nodiscard]] const ArbiterConfig& config() const noexcept { return config_; }

    /// Register a subsystem. Declaring the same subsystem twice replaces its declaration, which is
    /// what a profile change does.
    Status declare(const SubsystemDeclaration& declaration) noexcept;

    [[nodiscard]] bool registered(BudgetSubsystem subsystem) const noexcept;

    /// The measured total frame time. **This is the only place in the renderer where a frame time
    /// is accepted**, and it is why the arbiter is a singleton rather than an interface a subsystem
    /// could be handed.
    void report_frame_ms(f32 frame_ms) noexcept;

    /// One subsystem's own measured cost, where it currently stands on its ladder, and whether it
    /// has anything left to give. The last two come from the subsystem because the subsystem is
    /// what moved: an arbiter that assumed its grant was obeyed would price against a position
    /// nothing is at.
    void report_subsystem(BudgetSubsystem subsystem, f32 measured_ms, u8 position,
                          bool at_minimum) noexcept;

    /// "A pinned mode SHALL disable the arbiter and every subsystem controller together... Partial
    /// pinning SHALL NOT be possible." This class stops; propagating it to the controllers is the
    /// caller's single loop, and `ArbiterReport::pinned` is what that loop reads.
    void set_pinned(bool pinned) noexcept { pinned_ = pinned; }
    [[nodiscard]] bool pinned() const noexcept { return pinned_; }

    /// One frame. Arbitrates on one frame in `period_frames`; reports on every frame.
    [[nodiscard]] ArbiterReport update() noexcept;

    [[nodiscard]] f32 allocation_ms(BudgetSubsystem subsystem) const noexcept;
    [[nodiscard]] f32 resolution_scale() const noexcept;
    /// The allocatable budget: `frame_budget_ms - non_allocatable_ms`.
    [[nodiscard]] f32 allocatable_ms() const noexcept;

    /// Release: every allocation back to what position 0 costs, resolution scale back to 1.0. What
    /// a load spike ending, or a profile change, does.
    void restore_authored_quality() noexcept;

private:
    struct Entry {
        SubsystemDeclaration declaration;
        bool registered = false;
        f32 filtered_ms = 0.0F;
        /// The measured cost of position 0, learned from the reported cost and position.
        f32 scale_ms = 1.0F;
        f32 allocation_ms = 0.0F;
        u8 position = 0;
        bool at_minimum = false;
        bool measured = false;
        u32 frames_since_grant = 0;
    };

    [[nodiscard]] static f32 predicted_at(const Entry& entry, u8 position) noexcept;
    /// The coarsest single-step cost quantum reachable from where every subsystem stands now.
    [[nodiscard]] f32 coarsest_quantum_ms() const noexcept;
    /// What one step of resolution scale would add, priced through `resolution_sensitivity`.
    [[nodiscard]] f32 resolution_step_increase_ms(u8 target) const noexcept;
    void cap_and_floor(Entry& entry) const noexcept;
    void tighten(f32 error_ms, ArbiterReport& report) noexcept;
    void relax(f32 headroom_ms, ArbiterReport& report) noexcept;
    [[nodiscard]] bool everything_at_minimum() const noexcept;
    static void push(ArbiterReport& report, const BudgetAdjustment& adjustment) noexcept;
    /// Subsystem indices in ascending `reduction_order`, ties broken by enumerator. Recomputed on
    /// `declare()` so a tick never sorts.
    void rebuild_order() noexcept;

    ArbiterConfig config_;
    Entry entries_[kBudgetSubsystemCount];
    u8 order_[kBudgetSubsystemCount] = {};
    u32 order_count_ = 0;
    f32 filtered_frame_ms_ = 0.0F;
    f32 measured_frame_ms_ = 0.0F;
    u8 resolution_position_ = 0;
    u32 frames_since_resolution_grant_ = 0;
    u32 frame_counter_ = 0;
    bool frame_measured_ = false;
    bool pinned_ = false;
};

}  // namespace cy::rendering
