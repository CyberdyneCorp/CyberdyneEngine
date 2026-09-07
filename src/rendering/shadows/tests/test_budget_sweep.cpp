// The shadow budget's control law, certified over a SWEEP of loads. Task 8.2.
//
// ================================================================================================
// WHY A SWEEP AND NOT A STEP
// ================================================================================================
//
// M7 `design.md` §2.1, which is the arbiter spike's own methodological finding and is repeated here
// because this controller is the same shape one level down: "a control law over a discrete ladder
// MUST be certified over a sweep of loads, never one load. Where the equilibrium lands relative to
// a ladder boundary decides whether it oscillates. The spike's own first draft was certified on one
// load and oscillated on 27 of 71."
//
// So every case below runs 41 load magnitudes from x1.00 to x2.40, holds each for 800 frames, and
// counts every lever change in the last 450. A settled loop moves zero times, on every load.
//
// The cost model is deliberately the spike's shape and not a proxy for a GPU: measured cost is the
// nominal cost times the load times the product of the declared relative costs of the lever
// positions in force, plus +/-3% measurement noise. That is exactly the arithmetic the declared
// ladder promises, so what this test certifies is the CONTROL LAW — whether the loop settles — and
// not whether the ladder's numbers describe a real shadow pass. The second question needs a device
// and is not answerable here; it is stated as a limit rather than implied away.

#include <cy/test/test.h>

#include <cy/rendering/shadows/budget.h>

namespace {

using cy::rendering::ShadowBudget;
using cy::rendering::ShadowBudgetUpdate;
using cy::rendering::ShadowLever;

constexpr cy::f32 kAllocationMs = 2.00F;
constexpr cy::f32 kNominalMs = 2.00F;
constexpr cy::u32 kSweepPoints = 41;
constexpr cy::u32 kHoldFrames = 800;
constexpr cy::u32 kQuietFrames = 450;

/// A deterministic +/-3% measurement noise. An LCG rather than the engine's own generator so that
/// this certification is reproducible from the numbers written in this file alone.
class Noise {
public:
    explicit Noise(cy::u64 seed) noexcept : state_(seed) {}

    [[nodiscard]] cy::f32 next() noexcept {
        state_ = (state_ * 6364136223846793005ULL) + 1442695040888963407ULL;
        const auto bits = static_cast<cy::u32>(state_ >> 33U);
        const cy::f32 unit = static_cast<cy::f32>(bits) / 2147483648.0F;
        return 1.0F + ((unit - 0.5F) * 0.06F);
    }

private:
    cy::u64 state_;
};

/// What the subsystem costs at the lever positions currently in force.
[[nodiscard]] cy::f32 modelled_cost(const ShadowBudget& budget, cy::f32 load) noexcept {
    cy::f32 factor = 1.0F;
    for (cy::u32 index = 0; index < cy::rendering::kShadowLeverCount; ++index) {
        const cy::rendering::ShadowLeverLadder& ladder = budget.config().levers[index];
        if (!ladder.declared) {
            continue;
        }
        const auto lever = static_cast<ShadowLever>(index);
        factor *= ladder.relative_cost[budget.position(lever)];
    }
    return kNominalMs * load * factor;
}

struct RunResult {
    cy::u32 changes_in_quiet_window = 0;
    cy::u32 total_changes = 0;
    cy::f32 settled_ms = 0.0F;
    bool ever_below_zero_quality = false;
};

/// One load, held. `arbiter_period` and `grant_spacing` are the arbiter's half of the loop,
/// modelled here because a controller may never relax on its own authority (`design.md` §2.6): the
/// grant has to come from outside the subsystem, so the test plays the outside.
[[nodiscard]] RunResult hold(ShadowBudget& budget, cy::f32 load, cy::u64 seed) noexcept {
    Noise noise(seed);
    RunResult result;
    cy::u32 last_grant = 0;
    for (cy::u32 frame = 1; frame <= kHoldFrames; ++frame) {
        budget.report_measured_ms(modelled_cost(budget, load) * noise.next());
        const ShadowBudgetUpdate update = budget.update();

        if (update.tightened || update.relaxed) {
            ++result.total_changes;
            if (frame > kHoldFrames - kQuietFrames) {
                ++result.changes_in_quiet_window;
            }
        }
        result.settled_ms = update.filtered_ms;

        // The arbiter: every 8 frames, and never within 18 frames of its last grant.
        if (frame % 8 == 0 && frame >= last_grant + 18 &&
            update.filtered_ms <= kAllocationMs * budget.config().relax_margin) {
            budget.grant_relax_step();
            last_grant = frame;
        }
    }
    // A frame is coarser, never missing: every ladder's last position still has a quality.
    for (const cy::rendering::ShadowLeverLadder& ladder : budget.config().levers) {
        if (ladder.declared && ladder.relative_cost[ladder.positions - 1U] <= 0.0F) {
            result.ever_below_zero_quality = true;
        }
    }
    return result;
}

[[nodiscard]] cy::f32 load_at(cy::u32 point) noexcept {
    return 1.00F + (1.40F * static_cast<cy::f32>(point) / static_cast<cy::f32>(kSweepPoints - 1U));
}

}  // namespace

CY_TEST_CASE(
    "the shadow controller settles on every load in the sweep, and none settles over budget") {
    cy::u32 oscillating = 0;
    cy::u32 worst_quiet_changes = 0;
    cy::u32 over_budget = 0;

    for (cy::u32 point = 0; point < kSweepPoints; ++point) {
        const cy::f32 load = load_at(point);
        ShadowBudget budget;
        budget.set_allocation_ms(kAllocationMs);

        const RunResult result = hold(budget, load, 0x9E3779B97F4A7C15ULL + point);
        if (result.changes_in_quiet_window != 0) {
            ++oscillating;
            worst_quiet_changes = result.changes_in_quiet_window > worst_quiet_changes
                                      ? result.changes_in_quiet_window
                                      : worst_quiet_changes;
        }
        if (result.settled_ms > kAllocationMs * budget.config().tighten_margin) {
            ++over_budget;
        }
        CY_CHECK_FALSE(result.ever_below_zero_quality);
    }

    CY_CHECK_EQ(oscillating, 0U);
    CY_CHECK_EQ(worst_quiet_changes, 0U);
    CY_CHECK_EQ(over_budget, 0U);
}

CY_TEST_CASE("without the margin the controller ratchets quality away and never gives it back") {
    // `design.md` §2.7. Because tightening is unconditional and relaxing is arbitrated, a tighten
    // test with no margin loses a step to every noise excursion and never gets it back. It does not
    // look like oscillation; it looks like a scene that mysteriously gets coarser the longer you
    // stand still. This case is the measurement that keeps the 1.06 honest: if the shipped margin
    // ever stops mattering, the first half of this assertion fails.
    const cy::f32 nominal_load = 1.0F;  // exactly inside the allocation. Nothing should move.

    ShadowBudget shipped;
    shipped.set_allocation_ms(kAllocationMs);
    Noise shipped_noise(4242);
    for (cy::u32 frame = 0; frame < kHoldFrames; ++frame) {
        shipped.report_measured_ms(modelled_cost(shipped, nominal_load) * shipped_noise.next());
        (void)shipped.update();
    }
    for (cy::u32 index = 0; index < cy::rendering::kShadowLeverCount; ++index) {
        CY_CHECK_EQ(static_cast<cy::u32>(shipped.position(static_cast<ShadowLever>(index))), 0U);
    }

    ShadowBudget marginless;
    cy::rendering::ShadowBudgetConfig config = cy::rendering::default_shadow_budget_config();
    config.tighten_margin = 1.0F;
    marginless.configure(config);
    marginless.set_allocation_ms(kAllocationMs);
    Noise marginless_noise(4242);
    cy::u32 steps = 0;
    for (cy::u32 frame = 0; frame < kHoldFrames; ++frame) {
        marginless.report_measured_ms(modelled_cost(marginless, nominal_load) *
                                      marginless_noise.next());
        if (marginless.update().tightened) {
            ++steps;
        }
    }
    // The same scene, the same noise, the same allocation — and quality has walked down the ladder
    // for no reason but measurement noise. One step is the whole finding: it is unrecoverable
    // without a grant the arbiter has no reason to issue, because the subsystem is now comfortably
    // inside its allocation and looks perfectly healthy.
    CY_CHECK_GT(steps, 0U);
    CY_CHECK_GT(static_cast<cy::u32>(marginless.position(ShadowLever::Refinement)),
                static_cast<cy::u32>(shipped.position(ShadowLever::Refinement)));
}

CY_TEST_CASE("starved on every load, the subsystem reaches its minimum and stays there") {
    // The degradation criterion: "every paged system degrades along its declared axis… a frame is
    // never missing, only coarser". At x4 load every lever reaches its last position, the
    // controller says so, and it stops moving.
    ShadowBudget budget;
    budget.set_allocation_ms(kAllocationMs);

    Noise noise(12345);
    ShadowBudgetUpdate update;
    for (cy::u32 frame = 0; frame < 400; ++frame) {
        budget.report_measured_ms(modelled_cost(budget, 8.0F) * noise.next());
        update = budget.update();
    }
    CY_CHECK(update.at_minimum);
    CY_CHECK(budget.at_minimum());
    CY_CHECK_FALSE(update.tightened);
    CY_CHECK_FALSE(update.relaxed);
    for (cy::u32 index = 0; index < cy::rendering::kShadowLeverCount; ++index) {
        const auto lever = static_cast<ShadowLever>(index);
        const cy::rendering::ShadowLeverLadder& ladder = budget.config().levers[index];
        CY_CHECK_EQ(static_cast<cy::u32>(budget.position(lever)),
                    static_cast<cy::u32>(ladder.positions - 1U));
        // Coarser, not absent: the last position of every ladder still costs something and still
        // produces a shadow.
        CY_CHECK_GT(ladder.relative_cost[ladder.positions - 1U], 0.0F);
    }
}

CY_TEST_CASE("pinned mode stops everything and reports the overrun instead of correcting it") {
    ShadowBudget budget;
    budget.set_allocation_ms(kAllocationMs);
    budget.set_pinned(true);

    ShadowBudgetUpdate update;
    for (cy::u32 frame = 0; frame < 300; ++frame) {
        budget.report_measured_ms(modelled_cost(budget, 3.0F));
        update = budget.update();
        CY_CHECK_FALSE(update.tightened);
        CY_CHECK_FALSE(update.relaxed);
    }
    CY_CHECK_GT(update.pinned_overrun_ms, 0.0F);
    for (cy::u32 index = 0; index < cy::rendering::kShadowLeverCount; ++index) {
        CY_CHECK_EQ(static_cast<cy::u32>(budget.position(static_cast<ShadowLever>(index))), 0U);
    }
}

CY_TEST_CASE("released from pressure, authored quality comes back on every load") {
    for (cy::u32 point = 0; point < kSweepPoints; point += 4) {
        ShadowBudget budget;
        budget.set_allocation_ms(kAllocationMs);
        (void)hold(budget, load_at(point), 777 + point);

        // The load goes away. The arbiter grants steps back at its own pace; nothing is forced.
        Noise noise(999 + point);
        cy::u32 last_grant = 0;
        for (cy::u32 frame = 1; frame <= 1200; ++frame) {
            budget.report_measured_ms(modelled_cost(budget, 0.5F) * noise.next());
            const ShadowBudgetUpdate update = budget.update();
            if (frame % 8 == 0 && frame >= last_grant + 18 &&
                update.filtered_ms <= kAllocationMs * budget.config().relax_margin) {
                budget.grant_relax_step();
                last_grant = frame;
            }
        }
        for (cy::u32 index = 0; index < cy::rendering::kShadowLeverCount; ++index) {
            CY_CHECK_EQ(static_cast<cy::u32>(budget.position(static_cast<ShadowLever>(index))), 0U);
        }
    }
}

CY_TEST_CASE("the reserved minimum is declared and reported rather than silently crossed") {
    ShadowBudget budget;
    cy::rendering::ShadowBudgetConfig config = cy::rendering::default_shadow_budget_config();
    config.reserved_minimum_ms = 0.5F;
    budget.configure(config);

    budget.set_allocation_ms(1.0F);
    budget.report_measured_ms(1.0F);
    CY_CHECK_FALSE(budget.update().at_reserved_minimum);

    budget.set_allocation_ms(0.4F);
    budget.report_measured_ms(1.0F);
    CY_CHECK(budget.update().at_reserved_minimum);
}
