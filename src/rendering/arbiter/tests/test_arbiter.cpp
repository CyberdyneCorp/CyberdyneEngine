// The arbiter's own invariants: what it refuses, what its actuator does, where its deadband comes
// from, that a controller cannot relax on its own, and that pinning is total. The CRITERION of task
// 10.1 — convergence without oscillation — is `test_sweep.cpp`, because it takes 71 loads.

#include <cy/test/test.h>

#include <cy/rendering/arbiter/arbiter.h>
#include <cy/rendering/arbiter/profiles.h>
#include <cy/rendering/arbiter/subsystem.h>

namespace {

using cy::f32;
using cy::u32;
using cy::u8;
using cy::rendering::AdjustmentCause;
using cy::rendering::ArbiterConfig;
using cy::rendering::ArbiterReport;
using cy::rendering::BudgetAdjustment;
using cy::rendering::BudgetArbiter;
using cy::rendering::BudgetSubsystem;
using cy::rendering::kBudgetSubsystemCount;
using cy::rendering::SubsystemController;
using cy::rendering::SubsystemDeclaration;
using cy::rendering::SubsystemUpdate;

SubsystemDeclaration declaration(BudgetSubsystem subsystem, u32 order, f32 base_ms) noexcept {
    SubsystemDeclaration declared;
    declared.subsystem = subsystem;
    declared.reduction_order = order;
    declared.base_cost_ms = base_ms;
    declared.reserved_minimum_ms = base_ms * 0.2F;
    declared.ladder.positions = 4;
    declared.ladder.relative_cost[0] = 1.00F;
    declared.ladder.relative_cost[1] = 0.70F;
    declared.ladder.relative_cost[2] = 0.48F;
    declared.ladder.relative_cost[3] = 0.32F;
    return declared;
}

/// Drive the arbiter to a tick with a fixed frame cost, reporting each subsystem at its stated
/// position. Returns the tick's report.
ArbiterReport run_to_tick(BudgetArbiter& arbiter, f32 frame_ms, const f32* subsystem_ms,
                          const u8* positions, const bool* at_minimum, u32 frames) noexcept {
    ArbiterReport report;
    for (u32 frame = 0; frame < frames; ++frame) {
        arbiter.report_frame_ms(frame_ms);
        for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
            if (!arbiter.registered(static_cast<BudgetSubsystem>(index))) {
                continue;
            }
            arbiter.report_subsystem(static_cast<BudgetSubsystem>(index), subsystem_ms[index],
                                     positions[index], at_minimum[index]);
        }
        report = arbiter.update();
    }
    return report;
}

}  // namespace

CY_TEST_CASE("arbiter: a configuration that cannot be arbitrated is refused") {
    BudgetArbiter arbiter;

    ArbiterConfig bad_budget;
    bad_budget.non_allocatable_ms = bad_budget.frame_budget_ms;
    CY_CHECK_FALSE(arbiter.configure(bad_budget));

    ArbiterConfig bad_period;
    bad_period.period_frames = 0;
    CY_CHECK_FALSE(arbiter.configure(bad_period));

    ArbiterConfig bad_resolution;
    bad_resolution.resolution_scale[1] = 1.10F;  // must descend from 1.0
    CY_CHECK_FALSE(arbiter.configure(bad_resolution));

    ArbiterConfig good;
    CY_CHECK(arbiter.configure(good));
}

CY_TEST_CASE("arbiter: a ladder whose price rises along it is refused, by both halves") {
    SubsystemDeclaration rising = declaration(BudgetSubsystem::Geometry, 0, 2.0F);
    rising.ladder.relative_cost[2] = 0.90F;  // above position 1's 0.70

    SubsystemController controller;
    CY_CHECK_FALSE(controller.declare(rising));

    // The arbiter runs the controller's own check rather than a second copy of it, so a
    // declaration cannot be accepted by one and refused by the other.
    BudgetArbiter arbiter;
    CY_REQUIRE(arbiter.configure(ArbiterConfig{}));
    CY_CHECK_FALSE(arbiter.declare(rising));

    SubsystemDeclaration not_authored = declaration(BudgetSubsystem::Geometry, 0, 2.0F);
    not_authored.ladder.relative_cost[0] = 0.9F;
    CY_CHECK_FALSE(arbiter.declare(not_authored));
}

CY_TEST_CASE("arbiter: the actuator is the declared reduction order, one step per subsystem") {
    BudgetArbiter arbiter;
    ArbiterConfig config;
    config.frame_budget_ms = 10.0F;
    config.non_allocatable_ms = 1.0F;
    config.period_frames = 1;  // tick every frame so the test reads one decision at a time
    CY_REQUIRE(arbiter.configure(config));

    // Three subsystems, declared out of enumerator order on purpose: post-processing reduces
    // first (order 0) and geometry last (order 2), and geometry's enumerator is the LOWEST. An
    // arbiter walking the enumeration would take geometry first and this case would fail.
    CY_REQUIRE(arbiter.declare(declaration(BudgetSubsystem::Geometry, 2, 3.0F)));
    CY_REQUIRE(arbiter.declare(declaration(BudgetSubsystem::Shadows, 1, 3.0F)));
    CY_REQUIRE(arbiter.declare(declaration(BudgetSubsystem::PostProcessing, 0, 3.0F)));

    f32 costs[kBudgetSubsystemCount] = {};
    u8 positions[kBudgetSubsystemCount] = {};
    bool minimum[kBudgetSubsystemCount] = {};
    costs[static_cast<u32>(BudgetSubsystem::Geometry)] = 3.0F;
    costs[static_cast<u32>(BudgetSubsystem::Shadows)] = 3.0F;
    costs[static_cast<u32>(BudgetSubsystem::PostProcessing)] = 3.0F;

    // 12 ms against a 10 ms budget: over by 2, of which the gain covers 0.35 * (2 + deadband).
    // One step of any of these three saves 0.9 ms, so the first subsystem alone covers it.
    const ArbiterReport report = run_to_tick(arbiter, 12.0F, costs, positions, minimum, 1);
    CY_REQUIRE(report.arbitrated);
    CY_REQUIRE_EQ(report.adjustment_count, 1U);
    CY_CHECK_EQ(report.adjustments[0].subsystem, BudgetSubsystem::PostProcessing);
    CY_CHECK_EQ(report.adjustments[0].cause, AdjustmentCause::FrameOverBudget);
    CY_CHECK_EQ(report.adjustments[0].to_position, 1);
    // Nothing was scaled: the two subsystems the walk did not reach still hold their allocations.
    CY_CHECK_GT(arbiter.allocation_ms(BudgetSubsystem::Geometry), 3.0F);
    CY_CHECK_GT(arbiter.allocation_ms(BudgetSubsystem::Shadows), 3.0F);
}

CY_TEST_CASE("arbiter: a deficit larger than one step walks further down the order") {
    BudgetArbiter arbiter;
    ArbiterConfig config;
    config.frame_budget_ms = 10.0F;
    config.non_allocatable_ms = 1.0F;
    config.period_frames = 1;
    CY_REQUIRE(arbiter.configure(config));
    CY_REQUIRE(arbiter.declare(declaration(BudgetSubsystem::Geometry, 2, 3.0F)));
    CY_REQUIRE(arbiter.declare(declaration(BudgetSubsystem::Shadows, 1, 3.0F)));
    CY_REQUIRE(arbiter.declare(declaration(BudgetSubsystem::PostProcessing, 0, 3.0F)));

    f32 costs[kBudgetSubsystemCount] = {};
    u8 positions[kBudgetSubsystemCount] = {};
    bool minimum[kBudgetSubsystemCount] = {};
    for (BudgetSubsystem subsystem :
         {BudgetSubsystem::Geometry, BudgetSubsystem::Shadows, BudgetSubsystem::PostProcessing}) {
        costs[static_cast<u32>(subsystem)] = 8.0F;
    }

    // 25 ms against 10: over by 15, gain covers 5.25 ms. One step of an 8 ms subsystem saves
    // 2.4 ms, so the walk must reach all three and stop there.
    const ArbiterReport report = run_to_tick(arbiter, 25.0F, costs, positions, minimum, 1);
    CY_REQUIRE_EQ(report.adjustment_count, 3U);
    CY_CHECK_EQ(report.adjustments[0].subsystem, BudgetSubsystem::PostProcessing);
    CY_CHECK_EQ(report.adjustments[1].subsystem, BudgetSubsystem::Shadows);
    CY_CHECK_EQ(report.adjustments[2].subsystem, BudgetSubsystem::Geometry);
}

CY_TEST_CASE("arbiter: resolution scale is the last lever and only when nothing else is left") {
    BudgetArbiter arbiter;
    ArbiterConfig config;
    config.frame_budget_ms = 10.0F;
    config.non_allocatable_ms = 1.0F;
    config.period_frames = 1;
    CY_REQUIRE(arbiter.configure(config));
    CY_REQUIRE(arbiter.declare(declaration(BudgetSubsystem::Geometry, 0, 4.0F)));

    f32 costs[kBudgetSubsystemCount] = {};
    u8 positions[kBudgetSubsystemCount] = {};
    bool minimum[kBudgetSubsystemCount] = {};
    costs[static_cast<u32>(BudgetSubsystem::Geometry)] = 20.0F;

    CY_TEST_SUBCASE("with a step left, resolution does not move") {
        positions[static_cast<u32>(BudgetSubsystem::Geometry)] = 1;
        const ArbiterReport report = run_to_tick(arbiter, 24.0F, costs, positions, minimum, 1);
        CY_CHECK_EQ(report.resolution_scale, 1.0F);
        CY_REQUIRE_EQ(report.adjustment_count, 1U);
        CY_CHECK_EQ(report.adjustments[0].subsystem, BudgetSubsystem::Geometry);
    }

    CY_TEST_SUBCASE("at the bottom of every ladder, resolution scale moves and says why") {
        positions[static_cast<u32>(BudgetSubsystem::Geometry)] = 3;
        minimum[static_cast<u32>(BudgetSubsystem::Geometry)] = true;
        const ArbiterReport report = run_to_tick(arbiter, 24.0F, costs, positions, minimum, 1);
        CY_CHECK_LT(report.resolution_scale, 1.0F);
        CY_REQUIRE_EQ(report.adjustment_count, 1U);
        CY_CHECK_EQ(report.adjustments[0].subsystem, BudgetSubsystem::Count);
        CY_CHECK_EQ(report.adjustments[0].cause, AdjustmentCause::EverySubsystemAtMinimum);
    }
}

CY_TEST_CASE("arbiter: the deadband is sized against the coarsest step reachable from here") {
    BudgetArbiter arbiter;
    ArbiterConfig config;
    config.frame_budget_ms = 20.0F;
    config.non_allocatable_ms = 1.0F;
    config.period_frames = 1;
    config.deadband_multiple = 0.5F;
    CY_REQUIRE(arbiter.configure(config));
    // A 10 ms subsystem whose first step saves 3.0 ms. Deadband = 1.5 ms; setpoint = 18.5 ms.
    CY_REQUIRE(arbiter.declare(declaration(BudgetSubsystem::Geometry, 0, 10.0F)));

    f32 costs[kBudgetSubsystemCount] = {};
    u8 positions[kBudgetSubsystemCount] = {};
    bool minimum[kBudgetSubsystemCount] = {};
    costs[static_cast<u32>(BudgetSubsystem::Geometry)] = 10.0F;

    const ArbiterReport report = run_to_tick(arbiter, 19.5F, costs, positions, minimum, 1);
    CY_CHECK_NEAR(report.deadband_ms, 1.5F, 0.02F);
    // The band's UPPER edge sits on the budget, which is why a settled frame is never over it.
    CY_CHECK_NEAR(report.setpoint_ms + report.deadband_ms, config.frame_budget_ms, 0.001F);
    // 19.5 ms is inside the band around the 18.5 ms setpoint, so nothing moves.
    CY_CHECK_EQ(report.adjustment_count, 0U);
}

CY_TEST_CASE("arbiter: pinned mode is total, and the overrun is reported rather than corrected") {
    BudgetArbiter arbiter;
    ArbiterConfig config;
    config.frame_budget_ms = 10.0F;
    config.non_allocatable_ms = 1.0F;
    config.period_frames = 1;
    CY_REQUIRE(arbiter.configure(config));
    CY_REQUIRE(arbiter.declare(declaration(BudgetSubsystem::Geometry, 0, 4.0F)));
    arbiter.set_pinned(true);

    f32 costs[kBudgetSubsystemCount] = {};
    u8 positions[kBudgetSubsystemCount] = {};
    bool minimum[kBudgetSubsystemCount] = {};
    costs[static_cast<u32>(BudgetSubsystem::Geometry)] = 8.0F;

    const f32 before = arbiter.allocation_ms(BudgetSubsystem::Geometry);
    const ArbiterReport report = run_to_tick(arbiter, 17.42F, costs, positions, minimum, 40);
    CY_CHECK(report.pinned);
    CY_CHECK_FALSE(report.arbitrated);
    CY_CHECK_EQ(report.adjustment_count, 0U);
    CY_CHECK_EQ(arbiter.allocation_ms(BudgetSubsystem::Geometry), before);
    CY_CHECK_NEAR(report.pinned_overrun_ms, 7.42F, 0.01F);

    // The controller half of "partial pinning SHALL NOT be possible": a pinned controller does not
    // tighten however far over its allocation it is.
    SubsystemController controller;
    CY_REQUIRE(controller.declare(declaration(BudgetSubsystem::Geometry, 0, 4.0F)));
    controller.set_pinned(true);
    controller.set_allocation_ms(1.0F);
    for (u32 frame = 0; frame < 40; ++frame) {
        controller.report_measured_ms(8.0F);
        const SubsystemUpdate update = controller.update();
        CY_CHECK_FALSE(update.tightened);
        CY_CHECK_GT(update.pinned_overrun_ms, 0.0F);
    }
    CY_CHECK_EQ(controller.position(), 0);
}

CY_TEST_CASE("controller: it tightens on its own authority and never relaxes on its own") {
    SubsystemController controller;
    CY_REQUIRE(controller.declare(declaration(BudgetSubsystem::Shadows, 0, 4.0F)));

    // Over its allocation: it fixes that now, with nobody's permission.
    controller.set_allocation_ms(2.0F);
    for (u32 frame = 0; frame < 8; ++frame) {
        controller.report_measured_ms(4.0F * controller.declaration().ladder.cost_at(
                                                 controller.position()));
        (void)controller.update();
    }
    CY_CHECK_GT(controller.position(), 0);
    const u8 tightened_to = controller.position();

    // Under its allocation by any margin, for as long as you like: it does not step back up,
    // because the time a step up costs comes out of a frame it is forbidden to see.
    controller.set_allocation_ms(40.0F);
    for (u32 frame = 0; frame < 400; ++frame) {
        controller.report_measured_ms(4.0F * controller.declaration().ladder.cost_at(
                                                 controller.position()));
        const SubsystemUpdate update = controller.update();
        CY_CHECK_FALSE(update.relaxed);
    }
    CY_CHECK_EQ(controller.position(), tightened_to);

    // One grant, one step. Not two.
    controller.grant_relax_step();
    u32 relaxations = 0;
    for (u32 frame = 0; frame < 200; ++frame) {
        controller.report_measured_ms(4.0F * controller.declaration().ladder.cost_at(
                                                 controller.position()));
        relaxations += controller.update().relaxed ? 1U : 0U;
    }
    CY_CHECK_EQ(relaxations, 1U);
    CY_CHECK_EQ(controller.position(), static_cast<u8>(tightened_to - 1U));
}

CY_TEST_CASE("controller: the tighten margin is what stops the ratchet, and it is measurable") {
    // `design.md` §2.7. Tightening is unconditional and relaxing is arbitrated, so a tighten test
    // with NO margin loses a step to every noise excursion and never gets it back. This case
    // measures the ratchet rather than asserting the constant: the same noise, the same allocation
    // that the subsystem is exactly inside, and the only difference is the margin.
    const auto run = [](f32 margin) {
        SubsystemController controller;
        cy::rendering::SubsystemControllerConfig config;
        config.tighten_margin = margin;
        CY_REQUIRE(controller.declare(declaration(BudgetSubsystem::Vfx, 0, 4.0F), config));
        controller.set_allocation_ms(4.0F);
        u32 seed = 0x1234567U;
        for (u32 frame = 0; frame < 600; ++frame) {
            seed ^= seed << 13U;
            seed ^= seed >> 17U;
            seed ^= seed << 5U;
            const f32 unit = static_cast<f32>(seed % 2001U) / 1000.0F - 1.0F;
            const f32 cost = 4.0F *
                             controller.declaration().ladder.cost_at(controller.position()) *
                             (1.0F + unit * 0.03F);
            controller.report_measured_ms(cost);
            (void)controller.update();
        }
        return controller.position();
    };

    CY_CHECK_GT(run(1.00F), 0);  // no margin: the scene quietly gets coarser
    CY_CHECK_EQ(run(1.06F), 0);  // twice the noise: it stays where it was authored
}
