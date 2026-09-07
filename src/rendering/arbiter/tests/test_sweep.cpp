// THE CRITERION OF TASK 10.1: the arbiter's allocations converge without oscillation under a step
// load, and every subsystem degrades along its declared axis rather than disappearing.
//
// Certified over a SWEEP of 71 step magnitudes, never one. `design.md` §2.1: the levers are a
// discrete ladder, so whether a control law oscillates depends on where its equilibrium lands
// relative to a ladder boundary — and one step magnitude can make any law look stable. The spike's
// own first draft was certified at one load and oscillated on 27 of the 71.
//
// Integration rather than unit: 71 loads x 1,400 frames x 7 subsystems does not fit a 1 ms unit
// budget, and a sweep shortened to fit one would certify something other than the criterion.

#include <cy/test/test.h>

#include "model.h"

namespace {

using cy::f32;
using cy::u32;
using cy::rendering::BudgetSubsystem;
using cy::rendering::kBudgetSubsystemCount;
using cy::rendering::named_profile;
using cy::rendering::ProfileName;
using cy::rendering::RendererProfile;
using cy::rendering::test::Renderer;
using cy::rendering::test::StepResult;

/// 71 loads from x1.00 to x2.40, which is the spike's own sweep.
constexpr u32 kLoadCount = 71;
constexpr f32 kLoadFirst = 1.00F;
constexpr f32 kLoadLast = 2.40F;
/// The plateau, and the tail of it over which a settled loop must move zero times.
constexpr u32 kPlateauFrames = 700;
constexpr u32 kTailFrames = 450;

[[nodiscard]] f32 load_at(u32 index) noexcept {
    return kLoadFirst + (kLoadLast - kLoadFirst) * static_cast<f32>(index) /
                            static_cast<f32>(kLoadCount - 1U);
}

struct PlateauResult {
    u32 tail_changes = 0;
    u32 settle_frame = 0;
    f32 final_true_frame_ms = 0.0F;
    bool everything_at_minimum = false;
};

/// Hold `load` for `kPlateauFrames` and report what the last `kTailFrames` did.
[[nodiscard]] PlateauResult hold(Renderer& renderer, f32 load) noexcept {
    renderer.set_load(load);
    PlateauResult result;
    for (u32 frame = 0; frame < kPlateauFrames; ++frame) {
        const StepResult step = renderer.step();
        if (step.lever_changed) {
            result.settle_frame = frame;
            if (frame >= kPlateauFrames - kTailFrames) {
                ++result.tail_changes;
            }
        }
        result.final_true_frame_ms = step.true_frame_ms;
    }
    result.everything_at_minimum = true;
    for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
        const auto subsystem = static_cast<BudgetSubsystem>(index);
        if (renderer.arbiter().registered(subsystem) &&
            !renderer.controller(subsystem).at_minimum()) {
            result.everything_at_minimum = false;
        }
    }
    return result;
}

}  // namespace

CY_TEST_CASE("arbiter sweep: 71 step loads settle, none oscillates, none settles over budget") {
    const RendererProfile profile = named_profile(ProfileName::Standard);
    const f32 budget = profile.arbiter.frame_budget_ms;

    u32 oscillating = 0;
    u32 worst_tail_changes = 0;
    u32 over_budget = 0;
    u32 settle_sum = 0;
    u32 worst_settle = 0;

    for (u32 index = 0; index < kLoadCount; ++index) {
        Renderer renderer;
        CY_REQUIRE(renderer.build(profile));

        // Settle at authored quality first, so the sweep measures the response to a STEP rather
        // than the response to being switched on.
        (void)hold(renderer, 1.0F);

        const PlateauResult plateau = hold(renderer, load_at(index));
        if (plateau.tail_changes > 0) {
            ++oscillating;
            worst_tail_changes = plateau.tail_changes > worst_tail_changes ? plateau.tail_changes
                                                                          : worst_tail_changes;
        }
        settle_sum += plateau.settle_frame;
        worst_settle = plateau.settle_frame > worst_settle ? plateau.settle_frame : worst_settle;

        // A settled frame is never over the budget: that is what the setpoint being one deadband
        // below the budget buys, and `design.md` §2.4 measures the alternative at 2 of 71.
        // Except where the whole ladder has been spent — then the frame is honestly over budget
        // and the arbiter's job is to report it, which the degradation case below asserts.
        if (!plateau.everything_at_minimum && plateau.final_true_frame_ms > budget) {
            ++over_budget;
        }
    }

    CY_TEST_MESSAGE("sweep: ", kLoadCount, " loads, ", oscillating, " oscillating (worst tail ",
                    worst_tail_changes, " changes), ", over_budget,
                    " settled over budget, mean settle frame ", settle_sum / kLoadCount,
                    ", worst ", worst_settle);

    CY_CHECK_EQ(oscillating, 0U);
    CY_CHECK_EQ(over_budget, 0U);
}

CY_TEST_CASE("arbiter sweep: authored quality returns on every load when the spike ends") {
    const RendererProfile profile = named_profile(ProfileName::Standard);
    u32 restored = 0;

    for (u32 index = 0; index < kLoadCount; index += 5U) {
        Renderer renderer;
        CY_REQUIRE(renderer.build(profile));
        (void)hold(renderer, 1.0F);
        (void)hold(renderer, load_at(index));

        // Release. Restoration is arbitrated one step per arbiter tick with an 18-frame dwell
        // between grants, so it takes longer than the descent by construction — which is the
        // point: relaxing answers a frame that is fine and there is no hurry.
        renderer.set_load(1.0F);
        for (u32 frame = 0; frame < 4000; ++frame) {
            (void)renderer.step();
        }

        bool authored = renderer.arbiter().resolution_scale() == 1.0F;
        for (u32 slot = 0; slot < kBudgetSubsystemCount; ++slot) {
            const auto subsystem = static_cast<BudgetSubsystem>(slot);
            if (renderer.arbiter().registered(subsystem) &&
                renderer.controller(subsystem).position() != 0) {
                authored = false;
            }
        }
        restored += authored ? 1U : 0U;
    }

    CY_CHECK_EQ(restored, (kLoadCount + 4U) / 5U);
}

CY_TEST_CASE("arbiter: starve everything at once and every subsystem degrades, none disappears") {
    // `design.md` §2.9. A frame is coarser, never missing: every ladder's last position has a
    // non-zero quality by construction, and no subsystem is allocated below its reserved minimum.
    const RendererProfile profile = named_profile(ProfileName::Standard);
    Renderer renderer;
    CY_REQUIRE(renderer.build(profile));
    (void)hold(renderer, 1.0F);

    renderer.set_load(4.0F);
    for (u32 frame = 0; frame < 2000; ++frame) {
        (void)renderer.step();
    }

    u32 at_minimum = 0;
    u32 registered = 0;
    for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
        const auto subsystem = static_cast<BudgetSubsystem>(index);
        if (!renderer.arbiter().registered(subsystem)) {
            continue;
        }
        ++registered;
        at_minimum += renderer.controller(subsystem).at_minimum() ? 1U : 0U;

        // Degraded along its DECLARED axis, and still present: the last ladder position costs a
        // fraction of position 0 and never zero.
        const auto& declaration = profile.subsystems[index];
        CY_CHECK_GT(declaration.ladder.cost_at(declaration.ladder.last_position()), 0.0F);
        // Never allocated below what it reserved.
        CY_CHECK_GE(renderer.arbiter().allocation_ms(subsystem) + 1e-4F,
                    declaration.reserved_minimum_ms);
    }
    CY_CHECK_EQ(at_minimum, registered);

    // Only then does resolution scale move, and it is still above zero: the frame is smaller, not
    // absent.
    CY_CHECK_LT(renderer.arbiter().resolution_scale(), 1.0F);
    CY_CHECK_GT(renderer.arbiter().resolution_scale(), 0.5F);

    // And the loop is still there at the bottom of every ladder rather than thrashing.
    u32 changes = 0;
    for (u32 frame = 0; frame < 450; ++frame) {
        changes += renderer.step().lever_changed ? 1U : 0U;
    }
    CY_CHECK_EQ(changes, 0U);
}

CY_TEST_CASE("arbiter: every controller running its own copy of the decision oscillates") {
    // The forbidden case, modelled and measured — `design.md` §2.6, and the specification's
    // "the VFX and post-processing controllers SHALL NOT independently reduce quality for a cost
    // they did not incur". Seven corrections for one error.
    //
    // The model is deliberately the arbiter's OWN law, seven times: each subsystem gets a private
    // arbiter that sees the whole frame time and the whole frame budget and registers only itself.
    // Every one of them is individually correct and every one of them tries to cover the entire
    // deficit alone. That is what "a controller measuring total frame time" amounts to once it is
    // written down, and it is why the specification forbids the measurement rather than asking
    // controllers to be modest.
    const RendererProfile profile = named_profile(ProfileName::Standard);
    constexpr f32 kLoad = 1.6F;

    // The disciplined arrangement, for the comparison: one arbiter, seven controllers.
    Renderer disciplined;
    CY_REQUIRE(disciplined.build(profile));
    (void)hold(disciplined, 1.0F);
    const PlateauResult held = hold(disciplined, kLoad);

    // The forbidden one.
    cy::rendering::BudgetArbiter private_arbiter[kBudgetSubsystemCount];
    cy::rendering::SubsystemController rogue[kBudgetSubsystemCount];
    for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
        if (!profile.registered[index]) {
            continue;
        }
        CY_REQUIRE(private_arbiter[index].configure(profile.arbiter));
        CY_REQUIRE(private_arbiter[index].declare(profile.subsystems[index]));
        CY_REQUIRE(rogue[index].declare(profile.subsystems[index]));
    }

    u32 rogue_changes = 0;
    for (u32 frame = 0; frame < kPlateauFrames; ++frame) {
        f32 total = profile.arbiter.non_allocatable_ms;
        f32 own[kBudgetSubsystemCount] = {};
        for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
            if (!profile.registered[index]) {
                continue;
            }
            own[index] = profile.subsystems[index].base_cost_ms * kLoad *
                         profile.subsystems[index].ladder.cost_at(rogue[index].position());
            total += own[index];
        }
        for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
            if (!profile.registered[index]) {
                continue;
            }
            const auto subsystem = static_cast<BudgetSubsystem>(index);
            // THE FORBIDDEN LINE: this controller's arbiter is given the whole frame.
            private_arbiter[index].report_frame_ms(total);
            private_arbiter[index].report_subsystem(subsystem, own[index], rogue[index].position(),
                                                    rogue[index].at_minimum());
            rogue[index].report_measured_ms(own[index]);
            const auto report = private_arbiter[index].update();
            rogue[index].set_allocation_ms(report.allocation_ms[index]);
            if (report.relax_granted[index]) {
                rogue[index].grant_relax_step();
            }
            const auto update = rogue[index].update();
            if (frame >= kPlateauFrames - kTailFrames) {
                rogue_changes += (update.tightened || update.relaxed) ? 1U : 0U;
            }
        }
    }

    CY_TEST_MESSAGE("tail lever changes over ", kTailFrames, " frames at load ", kLoad,
                    ": one arbiter ", held.tail_changes, ", seven private arbiters ",
                    rogue_changes);
    CY_CHECK_EQ(held.tail_changes, 0U);
    CY_CHECK_GT(rogue_changes, 0U);
}
