// A SUBSYSTEM CONTROLLER UNDER `src/` REPORTS A MEASURED COST, AND A CONSTANT IS NOT ONE.
// M11.c task 5.3, `rendering-architecture`.
//
// The requirement this suite exists for was added by M11.c because the one already in the row —
// "Subsystem controllers SHALL report their measured cost to the arbiter" — was satisfied by
// nothing:
//
//   "A subsystem controller SHALL report a cost derived from an OBSERVATION of its own work in a
//   frame — a timer, a counter scaled by a measured unit cost, or a device query — and SHALL NOT
//   report a value that is a compile-time constant or a table lookup independent of the frame. The
//   arbiter's per-frame report SHALL distinguish a measured cost from an estimated one."
//
// ================================================================================================
// WHY THIS SUITE LINKS THE SKY
// ================================================================================================
//
// Because the requirement's second scenario says "at least one controller OUTSIDE A SAMPLE", and
// the check has to be able to tell the difference. Every `SubsystemController` in this tree before
// M11.c lived in `samples/07-fidelity` over a hard-coded cost table; a suite that declared its own
// controller here and fed it numbers would certify the arbiter and re-enact the defect one file
// over. `cy::rendering::sky::SkyBudget` is an ENGINE subsystem's controller — it is compiled into
// `cy::rendering-sky`, it is reached from `src/` and from nowhere else, and what it reports is the
// sky's own sample counts converted by a unit cost it learned from a clock.
//
// ================================================================================================
// WHAT IS NOT ASSERTED, AND IT IS THE WALL CLOCK
// ================================================================================================
//
// No case here asserts how long anything took. A unit case has a one-millisecond budget and a
// number read off a shared machine is not a property of this repository. What IS asserted is the
// RELATION the measurement establishes — that the reported cost moves with the work, that a
// controller which reports the same number forever is called an estimate, and that the allocation
// the arbiter hands back differs when the measured cost differs. `observe()` takes the elapsed time
// as an argument for exactly that reason, and the one case that does use the real clock
// (`begin_frame`/`end_frame`) asserts only that it produced a unit cost at all.

#include <cy/test/test.h>

#include <cy/rendering/arbiter/arbiter.h>
#include <cy/rendering/arbiter/subsystem.h>
#include <cy/rendering/sky/budget.h>

namespace {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::rendering::ArbiterConfig;
using cy::rendering::ArbiterReport;
using cy::rendering::BudgetArbiter;
using cy::rendering::BudgetSubsystem;
using cy::rendering::CostSource;
using cy::rendering::SubsystemController;
using cy::rendering::SubsystemDeclaration;
using cy::rendering::sky::kSkyBudgetSubsystem;
using cy::rendering::sky::SkyBudget;
using cy::rendering::sky::SkyWorkload;

/// One frame of sky work at a given size, as the counters would come back from the march.
[[nodiscard]] SkyWorkload workload_of(u64 rays, u32 steps) {
    SkyWorkload work;
    work.cloud_density_samples = rays * steps;
    work.cloud_light_samples = rays * (steps / 4U);
    work.shadow_density_samples = 4096;
    work.sky_view_rows = 4;
    return work;
}

/// A machine, stated rather than measured, so that a unit case is a relation and not a benchmark.
/// The value is irrelevant: every assertion below is about how the reported cost MOVES.
inline constexpr u64 kNanosecondsPerSample = 12;

[[nodiscard]] u64 elapsed_for(const SkyWorkload& work) {
    return work.samples() * kNanosecondsPerSample;
}

}  // namespace

CY_TEST_CASE(
    "the arbiter is fed measured costs by a controller outside a sample, and a constant is not one") {
    BudgetArbiter arbiter;
    CY_REQUIRE(arbiter.configure(ArbiterConfig{}));

    SkyBudget sky;
    CY_REQUIRE(sky.declare(arbiter));
    CY_CHECK(sky.declared());
    CY_CHECK_EQ(sky.subsystem(), kSkyBudgetSubsystem);

    // BEFORE ANY OBSERVATION the controller is on its declared `base_cost_ms`, and the report says
    // so. That is not a failure state — it is what the arbiter has to run on until something
    // measures — and the whole point of `CostSource` is that it is visible as such.
    arbiter.report_frame_ms(10.0F);
    sky.submit(arbiter);
    ArbiterReport report = arbiter.update();
    CY_CHECK_EQ(report.cost_source[static_cast<u32>(kSkyBudgetSubsystem)], CostSource::Estimated);
    CY_CHECK_EQ(sky.cost_source(), CostSource::Estimated);

    // NOW IT OBSERVES. Two frames of DIFFERENT work: the reported cost has to follow the count,
    // because the count is what the sky did and the conversion is what the clock said it costs.
    const SkyWorkload small = workload_of(20000, 32);
    const SkyWorkload large = workload_of(20000, 192);
    CY_REQUIRE_NE(small.samples(), large.samples());

    sky.observe(small, elapsed_for(small));
    const f32 cheap_ms = sky.cost_ms();
    sky.observe(large, elapsed_for(large));
    const f32 dear_ms = sky.cost_ms();
    CY_TEST_MESSAGE("measured cost: ", small.samples(), " samples -> ", cheap_ms, " ms, ",
                    large.samples(), " samples -> ", dear_ms, " ms, unit ", sky.stats().unit_cost_ns,
                    " ns/sample");
    CY_CHECK_GT(cheap_ms, 0.0F);
    CY_CHECK_GT(dear_ms, cheap_ms);

    // AND THE REPORT NOW SAYS `Measured`, because the claim and the numbers agree. This is the
    // assertion the whole requirement reduces to, and the one that goes red when the controller
    // stops reporting what it observed.
    sky.submit(arbiter);
    report = arbiter.update();
    CY_CHECK_EQ(sky.cost_source(), CostSource::Measured);
    CY_CHECK_EQ(report.cost_source[static_cast<u32>(kSkyBudgetSubsystem)], CostSource::Measured);
    CY_TEST_MESSAGE("cost source: sky reports ",
                    cy::rendering::cost_source_name(
                        report.cost_source[static_cast<u32>(kSkyBudgetSubsystem)]));

    // A CONSTANT IS NOT A MEASUREMENT, and it is the arbiter that says so rather than a reviewer.
    // This controller CLAIMS a measurement — it calls `report_measured_ms` — and reports the same
    // number in every frame, which is precisely the seven in `samples/07-fidelity` over their cost
    // table. The report marks it estimated.
    SubsystemDeclaration tabulated;
    tabulated.subsystem = BudgetSubsystem::Vfx;
    tabulated.reduction_order = 3;
    tabulated.base_cost_ms = 0.6F;
    tabulated.ladder.positions = 3;
    tabulated.ladder.relative_cost[0] = 1.0F;
    tabulated.ladder.relative_cost[1] = 0.66F;
    tabulated.ladder.relative_cost[2] = 0.40F;
    CY_REQUIRE(arbiter.declare(tabulated));
    SubsystemController constant_controller;
    CY_REQUIRE(constant_controller.declare(tabulated));
    for (u32 frame = 0; frame < 12; ++frame) {
        constant_controller.report_measured_ms(0.6F);
        arbiter.report_subsystem(BudgetSubsystem::Vfx, constant_controller.filtered_ms(),
                                 constant_controller.position(), constant_controller.at_minimum(),
                                 constant_controller.cost_source());
    }
    const ArbiterReport tabulated_report = arbiter.update();
    CY_CHECK_EQ(constant_controller.cost_source(), CostSource::Estimated);
    CY_CHECK_EQ(tabulated_report.cost_source[static_cast<u32>(BudgetSubsystem::Vfx)],
                CostSource::Estimated);

    // AND THE TWO ARE DISTINGUISHABLE IN ONE REPORT, which is the sentence the requirement adds to
    // the row: "so that a subsystem with no measurement is visible as such rather than
    // indistinguishable from one that is cheap".
    CY_CHECK_NE(tabulated_report.cost_source[static_cast<u32>(kSkyBudgetSubsystem)],
                tabulated_report.cost_source[static_cast<u32>(BudgetSubsystem::Vfx)]);
}

CY_TEST_CASE("the arbiter is fed measured costs and the allocation responds to them changing") {
    // TWO RUNS OF ONE FRAME SEQUENCE, identical in everything except what the sky measured. If the
    // allocation the sky receives is the same in both, the arbiter is arbitrating over a
    // declaration and the measurement is decoration.
    const auto run = [](u32 steps, f32& out_allocation, u32& out_position) {
        BudgetArbiter arbiter;
        CY_REQUIRE(arbiter.configure(ArbiterConfig{}));
        SkyBudget sky;
        CY_REQUIRE(sky.declare(arbiter));

        const SkyWorkload work = workload_of(20000, steps);
        for (u32 frame = 0; frame < 96; ++frame) {
            sky.observe(work, elapsed_for(work));
            // The frame is over budget by the same amount in both runs: what differs between them
            // is only what the SKY says it cost, which is the variable under test.
            arbiter.report_frame_ms(18.0F);
            sky.submit(arbiter);
            const ArbiterReport report = arbiter.update();
            const auto update = sky.apply(report);
            (void)update;
        }
        out_allocation = arbiter.allocation_ms(kSkyBudgetSubsystem);
        out_position = sky.controller().position();
    };

    f32 cheap_allocation = 0.0F;
    f32 dear_allocation = 0.0F;
    u32 cheap_position = 0;
    u32 dear_position = 0;
    run(32, cheap_allocation, cheap_position);
    run(192, dear_allocation, dear_position);

    CY_TEST_MESSAGE("allocation follows the measurement: cheap sky ", cheap_allocation,
                    " ms at position ", cheap_position, ", expensive sky ", dear_allocation,
                    " ms at position ", dear_position);
    CY_CHECK_GT(dear_allocation, cheap_allocation);
}

CY_TEST_CASE("the arbiter is fed measured costs taken from a clock rather than from a table") {
    // THE TIMER PATH, run once. It asserts no duration — see this file's header — only that timing
    // a frame's own work produced a unit cost, which is the half `observe()` cannot demonstrate
    // because its caller supplies the elapsed time.
    BudgetArbiter arbiter;
    CY_REQUIRE(arbiter.configure(ArbiterConfig{}));
    SkyBudget sky;
    CY_REQUIRE(sky.declare(arbiter));

    sky.begin_frame();
    sky.end_frame(workload_of(1000, 32));
    sky.begin_frame();
    sky.end_frame(workload_of(1000, 64));

    CY_TEST_MESSAGE("from the clock: unit ", sky.stats().unit_cost_ns, " ns/sample over ",
                    sky.stats().last_samples, " samples, last frame ", sky.stats().last_elapsed_ms,
                    " ms");
    CY_CHECK_GT(sky.stats().unit_cost_ns, 0.0F);
    CY_CHECK_EQ(sky.stats().observations, 2U);
    CY_CHECK_GT(sky.cost_ms(), 0.0F);

    // A frame that never started the clock is COUNTED rather than silently priced at nothing: "the
    // sky reported nothing this frame" and "the sky was free this frame" must not look the same.
    const u32 unpriced_before = sky.stats().unpriced;
    sky.end_frame(workload_of(1000, 32));
    CY_CHECK_EQ(sky.stats().unpriced, unpriced_before + 1U);
}

CY_TEST_CASE("the arbiter is fed measured costs and pinning stops the arbiter and the sky together") {
    // `rendering-architecture`: "A pinned mode SHALL disable the arbiter and every subsystem
    // controller together ... Partial pinning SHALL NOT be possible." The sky takes its pinned
    // state from the arbiter's report and from nowhere else, so a pinned arbiter and a moving sky
    // is not a state this code can reach — which is what task 5.4 needs before the artefact is
    // captured pinned.
    BudgetArbiter arbiter;
    ArbiterConfig config;
    config.frame_budget_ms = 13.90F;
    CY_REQUIRE(arbiter.configure(config));
    SkyBudget sky;
    CY_REQUIRE(sky.declare(arbiter));

    const SkyWorkload heavy = workload_of(20000, 192);
    for (u32 frame = 0; frame < 64; ++frame) {
        sky.observe(heavy, elapsed_for(heavy));
        arbiter.report_frame_ms(40.0F);
        sky.submit(arbiter);
        const ArbiterReport report = arbiter.update();
        (void)sky.apply(report);
    }
    const u32 degraded = sky.controller().position();
    CY_TEST_MESSAGE("unpinned, a 40 ms frame drove the sky to ladder position ", degraded, " (",
                    sky_quality_tier_name(sky.tier()), ")");
    CY_CHECK_GT(degraded, 0U);

    // Now pin, from a tier the arbiter did NOT choose, and run the same overrun.
    arbiter.restore_authored_quality();
    sky.restore_authored_quality();
    arbiter.set_pinned(true);
    for (u32 frame = 0; frame < 64; ++frame) {
        sky.observe(heavy, elapsed_for(heavy));
        arbiter.report_frame_ms(40.0F);
        sky.submit(arbiter);
        const ArbiterReport report = arbiter.update();
        CY_REQUIRE(report.pinned);
        const auto update = sky.apply(report);
        CY_CHECK_FALSE(update.tightened);
        CY_CHECK_FALSE(update.relaxed);
    }
    CY_CHECK_EQ(sky.controller().position(), 0U);
    CY_CHECK_GT(arbiter.update().pinned_overrun_ms, 0.0F);
}
