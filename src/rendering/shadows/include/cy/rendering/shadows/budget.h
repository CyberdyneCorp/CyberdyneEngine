#pragma once
// The shadow budget: one allocation, five declared levers, and a controller that never looks at the
// frame. Task 8.2.
//
// `virtual-shadows` — "Shadow budget", and M7 `design.md` §2, whose arbiter spike settled the
// control law this file implements the subsystem half of.
//
// ================================================================================================
// THERE IS NO WAY TO ASK THIS CLASS ABOUT FRAME TIME, AND THAT IS DELIBERATE
// ================================================================================================
//
// "The system SHALL measure and report its own cost and SHALL NOT measure total frame time."
// `rendering-architecture` says the same thing from the other side. The enforcement is the
// interface: `report_measured_ms()` takes the shadow subsystem's own cost and there is no second
// setter. A controller that wanted to react to the frame would have to be given the frame by
// somebody, and nobody can.
//
// The spike (`design.md` §2.6) found the half of that rule nobody writes down: **a controller may
// tighten on its own authority and may never relax on its own authority.** Overrunning its own
// allocation is its own business and it fixes that now. Stepping back UP costs time that comes out
// of the frame — the quantity it is forbidden to see — so a step back requires an explicit grant
// from the arbiter. `grant_relax_step()` is that grant, and it is worth ten lever changes per 450
// frames of a settled scene.
//
// ================================================================================================
// THE TWO MARGINS STRADDLE THE ALLOCATION, AND THAT IS WHAT STOPS THE RATCHET
// ================================================================================================
//
// Tighten at `measured > allocation * 1.06`; relax only when `predicted <= allocation * 0.88`.
// Because tightening is unconditional and relaxing is arbitrated, a tighten test with no margin
// loses a step to every noise excursion and never gets it back — which does not look like
// oscillation, it looks like a scene that mysteriously gets coarser the longer you stand still
// (`design.md` §2.7). The margins are the spike's measured values and the EMA alpha with them; the
// sweep in `test_budget.cpp` is what certifies them here, over 41 load magnitudes rather than one,
// because the levers are a discrete ladder and one step magnitude can make any law look stable.
//
// ================================================================================================
// PAGES ARE NEVER DROPPED. THEY GO STALE
// ================================================================================================
//
// There is deliberately no "pages per frame" lever. `virtual-shadows` requires the allocation to be
// spent "on the pages where staleness would be visible, rather than enforcing a fixed cap on pages
// per frame", and `pages.h` does that. What the levers here change is what a page COSTS — its
// geometry error, its filtering, its resolution, how often its class is refreshed — so the same
// allocation buys more pages rather than fewer.

#include <cy/core/base/types.h>
#include <cy/rendering/shadows/pages.h>

namespace cy::rendering {

/// The levers, declared in the order they are reduced. Lower reduces first, which is `residency`'s
/// meaning of `reduction_order` and is why the enumerator values are the order rather than a
/// separate table.
///
/// The order is the specification's: "Refinement SHALL be reduced before paged shadow quality is",
/// and "the shadow geometry error target SHALL be raised before pages are dropped". Resolution is
/// last because it is the one a viewer sees as a shadow going soft rather than as detail thinning.
enum class ShadowLever : u8 {
    /// Contact refinement — the screen-space or traced touch-up on top of the paged result.
    Refinement = 0,
    /// Filter kernel quality.
    FilterQuality,
    /// The shadow geometry error target, in shadow texels.
    GeometryError,
    /// How many frames a class of page may wait, multiplying `max_stale_frames`.
    RefreshInterval,
    /// A bias applied to selected page resolution. Protected receivers are exempt.
    PageResolution,
    Count,
};

[[nodiscard]] const char* shadow_lever_name(ShadowLever lever) noexcept;

inline constexpr u32 kShadowLeverCount = static_cast<u32>(ShadowLever::Count);
inline constexpr u32 kMaxLeverPositions = 4;

/// One lever's declared ladder. `relative_cost[i]` is what the subsystem costs at position `i` as a
/// fraction of what it costs at position 0 — the quantity `design.md` §2.10 identified as the one
/// thing a declared lever was missing, without which an arbiter allocating milliseconds over a
/// ladder is choosing blind.
///
/// **`residency::LeverSchedule` now carries a `relative_cost` of its own**, added by the arbiter's
/// section of this milestone, and the two are the same idea at different granularities rather than
/// two mechanisms:
///
///   * `residency`'s is indexed by `PressureLevel` — three positions — and covers the six levers
///     that structure names, two of which are shadow levers.
///   * this one is indexed by ladder POSITION, up to four, and covers the five levers
///     `virtual-shadows` makes budget levers, including geometry error and contact refinement,
///     which are not in `residency`'s enumeration and are not expressible as memory pressure.
///
/// The shadow budget is arbitrated in MILLISECONDS by the renderer's budget arbiter, not in bytes
/// by the memory pressure monitor, which is why the finer ladder lives here. Folding the two into
/// one declaration is worth doing and is a change to `residency`'s enumeration, not to this file —
/// so it is named here rather than done here, because that structure is not this module's to widen.
struct ShadowLeverLadder {
    bool declared = false;
    u8 positions = 1;
    /// Position 0 is authored quality and is 1.0 by construction. Later positions are cheaper.
    f32 relative_cost[kMaxLeverPositions] = {1.0F, 1.0F, 1.0F, 1.0F};
    /// What the lever reads at each position. Interpretation is the lever's own — texels for
    /// `GeometryError`, a multiplier for `RefreshInterval`, a level bias for `PageResolution`.
    f32 value[kMaxLeverPositions] = {0.0F, 0.0F, 0.0F, 0.0F};
};

struct ShadowBudgetConfig {
    /// What the subsystem costs at position 0 of every lever, in milliseconds. Used ONLY before
    /// the first measurement arrives: the ladder declares ratios and the controller measures the
    /// scale, so a declared absolute cost would be wrong the moment the scene changed.
    f32 base_cost_ms = 2.0F;
    /// The allocation below which the subsystem refuses to go. Reported when reached — "It SHALL
    /// declare a reserved minimum and report when it reaches it."
    f32 reserved_minimum_ms = 0.35F;
    /// `design.md` §2.2. Tighten above this multiple of the allocation.
    f32 tighten_margin = 1.06F;
    /// Relax only when the prediction is at or below this multiple.
    f32 relax_margin = 0.88F;
    /// EMA alpha on the measured cost.
    f32 filter_alpha = 0.25F;
    /// Frames between two steps back up, on the controller's own side of the grant.
    u32 relax_dwell_frames = 6;
    /// Receivers at or above this importance keep their quality while everything below degrades:
    /// "high-importance receivers SHALL retain shadow quality while background shadows degrade".
    f32 protected_importance = 0.8F;

    ShadowLeverLadder levers[kShadowLeverCount] = {};
};

/// A ladder set with the engine's defaults: every lever declared, with the relative costs the
/// shadow passes were measured at. A project overrides it; a test states what it changed.
[[nodiscard]] ShadowBudgetConfig default_shadow_budget_config() noexcept;

/// What one `update()` did. Every lever change is reported — "with each lever reported".
struct ShadowBudgetUpdate {
    bool tightened = false;
    bool relaxed = false;
    ShadowLever lever = ShadowLever::Count;
    u8 from_position = 0;
    u8 to_position = 0;
    /// Filtered own cost, in milliseconds.
    f32 filtered_ms = 0.0F;
    /// What the subsystem is predicted to cost at the current lever positions.
    f32 predicted_ms = 0.0F;
    /// True while every declared lever sits at its last position. The honest answer to "there is
    /// nothing left to give"; the arbiter reads it before it reaches for resolution scale.
    bool at_minimum = false;
    /// True when the allocation is at or below the declared reserved minimum.
    bool at_reserved_minimum = false;
    /// Milliseconds over the allocation this frame, when pinned. Reported rather than corrected —
    /// "Pinned mode is total" (`design.md` §2.9).
    f32 pinned_overrun_ms = 0.0F;
};

/// The subsystem controller. One per renderer; not thread-safe, stepped once per frame.
class ShadowBudget {
public:
    ShadowBudget() noexcept;

    void configure(const ShadowBudgetConfig& config) noexcept;

    [[nodiscard]] const ShadowBudgetConfig& config() const noexcept { return config_; }

    /// The arbiter's grant, in milliseconds. The only number from outside the subsystem.
    void set_allocation_ms(f32 allocation_ms) noexcept;

    [[nodiscard]] f32 allocation_ms() const noexcept { return allocation_ms_; }

    /// The subsystem's own measured cost. There is no frame-time equivalent, on purpose.
    void report_measured_ms(f32 measured_ms) noexcept;

    /// The arbiter permits one step back up. Consumed by the next `update()` that can use it.
    void grant_relax_step() noexcept;

    /// Pinned mode is global with the arbiter: everything stops together and overruns are reported.
    void set_pinned(bool pinned) noexcept;

    [[nodiscard]] bool pinned() const noexcept { return pinned_; }

    /// One frame of control. Returns what it changed, which is what the diagnostics print.
    [[nodiscard]] ShadowBudgetUpdate update() noexcept;

    [[nodiscard]] u8 position(ShadowLever lever) const noexcept;

    /// The lever's declared value at its current position — the number the passes read.
    [[nodiscard]] f32 value(ShadowLever lever) const noexcept;

    /// What the subsystem is predicted to cost at the current positions.
    [[nodiscard]] f32 predicted_ms() const noexcept;

    [[nodiscard]] bool at_minimum() const noexcept;

    /// Reset every lever to authored quality. What a release from pressure does.
    void restore_authored_quality() noexcept;

private:
    [[nodiscard]] f32 predicted_at(ShadowLever lever, u8 candidate) const noexcept;

    ShadowBudgetConfig config_;
    u8 positions_[kShadowLeverCount] = {};
    f32 allocation_ms_ = 0.0F;
    f32 filtered_ms_ = 0.0F;
    bool has_measurement_ = false;
    bool relax_granted_ = false;
    bool pinned_ = false;
    u32 frame_ = 0;
    u32 last_relax_frame_ = 0;
};

/// The page resolution the budget will actually pay for. A receiver at or above the configured
/// protected importance is exempt from the bias, which is the "hero shadows survive pressure"
/// scenario expressed as a function rather than as a promise.
[[nodiscard]] u8 apply_resolution_bias(const ShadowBudget& budget, u8 level, f32 importance,
                                       u8 max_level) noexcept;

/// How many frames a page of this class may wait, after the refresh-interval lever. `Critical`
/// answers zero at every lever position: it is not allowed to go stale, and no amount of pressure
/// changes that.
[[nodiscard]] u32 budgeted_stale_frames(const ShadowBudget& budget,
                                        UpdateClass update_class) noexcept;

}  // namespace cy::rendering
