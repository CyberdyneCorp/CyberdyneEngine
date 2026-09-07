#pragma once
// What a subsystem declares to the budget arbiter, and the controller that holds one allocation.
// Task 10.1.
//
// `rendering-architecture` — "Renderer budget arbiter". M7 `design.md` §2, whose spike settled the
// control law; this file is the SUBSYSTEM half of it and `arbiter.h` is the frame half.
//
// ================================================================================================
// A SUBSYSTEM DECLARES A LADDER AND ITS PRICE, NOT A NUMBER OF MILLISECONDS
// ================================================================================================
//
// `design.md` §2.10: "a subsystem must declare what each ladder position COSTS relative to
// position 0". Milliseconds are a property of the scene; the ratio is a property of the lever. So
// `QualityLadder::relative_cost` is a ratio, position 0 is 1.0 by construction, and the arbiter
// learns the SCALE from what the subsystem measures. `base_cost_ms` exists only for the frames
// before the first measurement arrives.
//
// This is `residency::LeverSchedule::relative_cost` one domain over: that one is indexed by
// `PressureLevel` and arbitrated in BYTES by the memory pressure monitor; this one is indexed by
// ladder position and arbitrated in MILLISECONDS by the renderer. Same idea, two currencies. A
// subsystem that is paged declares both, and `virtual-shadows`' `ShadowLeverLadder` is a third at a
// finer granularity again — five named levers where the arbiter sees one composite position.
//
// WHY THE ARBITER SEES ONE LADDER PER SUBSYSTEM AND NOT FIVE LEVERS. The arbiter allocates frame
// time; which of its own levers a subsystem moves to fit an allocation is the subsystem's decision
// and nobody else can make it well. The spike modelled seven subsystems with three- and four-
// position ladders for that reason. A subsystem with five levers folds them into a composite
// position — `ShadowBudget` already walks its levers in `reduction_order` internally — and declares
// the composite price here.
//
// ================================================================================================
// TIGHTENING IS LOCAL; RELAXING IS ARBITRATED
// ================================================================================================
//
// `rendering-architecture` forbids a controller to measure total frame time, and there is no
// setter here through which one could arrive. `design.md` §2.6 found the half nobody writes down:
// a controller may tighten on its own authority and may NEVER relax on its own authority, because
// the time a step back up costs comes out of the frame — the one quantity it is forbidden to see.
// `grant_relax_step()` is the arbiter's permission and it is consumed by one step.
//
// The two margins straddle the allocation for the reason `design.md` §2.7 measures: tightening is
// unconditional, so a tighten test with no margin loses a step to every noise excursion and never
// gets it back. That is not oscillation — it is a scene that quietly gets coarser the longer you
// stand still.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

namespace cy::rendering {

/// The arbiter's subsystems, in `rendering-architecture`'s own order: "geometry, shadows, global
/// illumination, reflections, material evaluation, VFX, post-processing, and resolution scale".
///
/// Resolution scale is deliberately NOT a member. It multiplies every raster-bound subsystem at
/// once, so it is not a peer allocation and cannot be arbitrated as one; it is the arbiter's last
/// lever, reached only when every subsystem here reports it is at its minimum (`design.md` §2.2).
///
/// This is not `residency::Subsystem` and must not become an alias of it. That enumeration is the
/// set of things that occupy BYTES — it has an audio entry and a world-cell entry and no entry for
/// post-processing — and this one is the set of things that spend MILLISECONDS. Three names appear
/// in both because three subsystems are both paged and expensive.
enum class BudgetSubsystem : u8 {
    Geometry = 0,
    Shadows,
    GlobalIllumination,
    Reflections,
    MaterialEvaluation,
    Vfx,
    PostProcessing,
    Count,
};

inline constexpr u32 kBudgetSubsystemCount = static_cast<u32>(BudgetSubsystem::Count);

[[nodiscard]] const char* budget_subsystem_name(BudgetSubsystem subsystem) noexcept;

/// The longest declared ladder. Four is what the spike's model used and five is what
/// `virtual-shadows` folds into a composite; six leaves room without making the arbiter's arrays
/// interesting.
inline constexpr u32 kMaxLadderPositions = 6;

/// A subsystem's quality ladder, priced. `relative_cost[p]` is what the subsystem costs at position
/// `p` as a fraction of what it costs at position 0.
struct QualityLadder {
    /// How many positions are real. 1 means "declared, and there is nothing to give" — which is a
    /// different statement from an undeclared subsystem, and the arbiter reports it differently.
    u8 positions = 1;
    f32 relative_cost[kMaxLadderPositions] = {1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};

    /// The price of position `p`, clamped into the declared range. Never zero and never negative:
    /// a position that cost nothing would let one step cover any deficit.
    [[nodiscard]] f32 cost_at(u8 position) const noexcept {
        // Clamped against BOTH the declared count and the array's own size. The second is not
        // redundant: `positions` is a field a caller sets, and a zero would make `positions - 1`
        // wrap to 255 and index a six-element array. `declare()` refuses a zero, but this is a
        // `constexpr`-friendly accessor a caller can reach before any declaration has happened.
        const u8 usable = last_position();
        const f32 cost = relative_cost[position < usable ? position : usable];
        return cost > 0.0F ? cost : 1.0F;
    }

    /// The last usable index. Never above `kMaxLadderPositions - 1`, and never wraps on a zero
    /// `positions`.
    [[nodiscard]] u8 last_position() const noexcept {
        const u8 count = positions > 0 ? positions : 1U;
        const u8 capped =
            count < kMaxLadderPositions ? count : static_cast<u8>(kMaxLadderPositions);
        return static_cast<u8>(capped - 1U);
    }
};

/// Everything one subsystem tells the arbiter, once, at registration.
struct SubsystemDeclaration {
    BudgetSubsystem subsystem = BudgetSubsystem::Geometry;

    /// Lower reduces first. `residency::SubsystemPolicy::reduction_order`'s meaning, kept
    /// deliberately — the arbiter's actuator IS this order (`design.md` §2.3) and restoration walks
    /// it backwards.
    u32 reduction_order = 0;

    /// "Each subsystem SHALL declare a reserved minimum." An allocation is never set below it, and
    /// reaching it is reported.
    f32 reserved_minimum_ms = 0.1F;

    /// What the subsystem costs at position 0 of its ladder, in milliseconds — used ONLY until the
    /// first measurement arrives. The ladder declares ratios; the arbiter measures the scale.
    f32 base_cost_ms = 1.0F;

    /// The fraction of this subsystem's cost that scales with pixel count, in [0, 1]. It is what
    /// lets the arbiter PRICE a step of resolution scale, which is otherwise the one lever whose
    /// effect it cannot predict — resolution multiplies every raster-bound subsystem at once, and
    /// an arbiter that assumed it multiplied all of them would over-credit a step whenever a
    /// compute-bound subsystem was expensive.
    ///
    /// 1.0 for a full-resolution raster pass, 0.0 for work whose cost is per instance or per light
    /// rather than per pixel — acceleration structure builds, GI cache updates on a fixed grid,
    /// instance culling. A subsystem that is half of each declares 0.5 and says why.
    f32 resolution_sensitivity = 1.0F;

    QualityLadder ladder;
};

/// The controller's own tuning. The values are the spike's (`design.md` §2.2) and a subsystem that
/// changes one should say why in its own code.
struct SubsystemControllerConfig {
    /// Tighten when `measured > allocation * tighten_margin`.
    f32 tighten_margin = 1.06F;
    /// Relax only when `predicted(next better) <= allocation * relax_margin`.
    f32 relax_margin = 0.88F;
    /// EMA alpha on the subsystem's own measured cost.
    f32 filter_alpha = 0.25F;
    /// Frames between two steps back up, on the controller's own side of the arbiter's grant.
    u32 relax_dwell_frames = 6;
};

/// What one `update()` did, and what a diagnostic prints. "Adjustments are attributable."
struct SubsystemUpdate {
    bool tightened = false;
    bool relaxed = false;
    u8 from_position = 0;
    u8 to_position = 0;
    /// Filtered own cost, in milliseconds.
    f32 filtered_ms = 0.0F;
    /// What the subsystem is predicted to cost at its current position.
    f32 predicted_ms = 0.0F;
    /// Every declared position is used up. The honest answer to "there is nothing left to give",
    /// and what the arbiter reads before it reaches for resolution scale.
    bool at_minimum = false;
    /// The allocation is at or below the declared reserved minimum.
    bool at_reserved_minimum = false;
    /// Milliseconds over the allocation while pinned — reported, never corrected.
    f32 pinned_overrun_ms = 0.0F;
};

/// One subsystem's controller: it holds an allocation using its own ladder, and it cannot see the
/// frame.
///
/// `virtual-shadows`' `ShadowBudget` and `rendering-global-illumination`'s `GiBudget` are two
/// hand-written instances of exactly this loop over their own named levers, written before this
/// class existed and kept because each maps a composite position onto levers only it understands.
/// This is the shape they share, for the subsystems whose ladder is genuinely one dimension —
/// material evaluation's tier, VFX's particle budget, post-processing's quality preset — and the
/// reference a new subsystem copies rather than re-deriving the margins.
class SubsystemController {
public:
    SubsystemController() noexcept = default;

    Status declare(const SubsystemDeclaration& declaration,
                   const SubsystemControllerConfig& config = {}) noexcept;

    [[nodiscard]] const SubsystemDeclaration& declaration() const noexcept { return declaration_; }

    /// The arbiter's grant, in milliseconds. The only number from outside the subsystem.
    void set_allocation_ms(f32 allocation_ms) noexcept;
    [[nodiscard]] f32 allocation_ms() const noexcept { return allocation_ms_; }

    /// The subsystem's own measured cost. There is no frame-time equivalent, on purpose.
    void report_measured_ms(f32 measured_ms) noexcept;

    /// The arbiter permits one step back up. Consumed by the next `update()` that can use it.
    void grant_relax_step() noexcept { relax_granted_ = true; }

    /// Pinned mode is global with the arbiter: everything stops together and overruns are reported.
    void set_pinned(bool pinned) noexcept { pinned_ = pinned; }
    [[nodiscard]] bool pinned() const noexcept { return pinned_; }

    [[nodiscard]] SubsystemUpdate update() noexcept;

    [[nodiscard]] u8 position() const noexcept { return position_; }
    [[nodiscard]] f32 filtered_ms() const noexcept { return filtered_ms_; }
    /// What the subsystem is predicted to cost at its current position.
    [[nodiscard]] f32 predicted_ms() const noexcept { return predicted_at(position_); }
    [[nodiscard]] bool at_minimum() const noexcept;

    /// Back to authored quality. What a release from pressure does.
    void restore_authored_quality() noexcept;

private:
    [[nodiscard]] f32 predicted_at(u8 candidate) const noexcept;

    SubsystemDeclaration declaration_;
    SubsystemControllerConfig config_;
    /// The measured cost of position 0, learned from `filtered_ms_ / relative_cost[position_]`.
    f32 scale_ms_ = 1.0F;
    f32 filtered_ms_ = 0.0F;
    f32 allocation_ms_ = 0.0F;
    u8 position_ = 0;
    u32 frames_since_relax_ = 0;
    bool measured_ = false;
    bool relax_granted_ = false;
    bool pinned_ = false;
};

}  // namespace cy::rendering
