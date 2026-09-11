// The parts of CyberVFX that are pure value work: precision selection, the budget controller's
// arithmetic, the event channels' two bounds, and the execution-path decision. M8.c section 2.
//
// UNIT TIER, AND EVERY CASE BELONGS THERE. Nothing here builds a world, a device, a particle system
// or a graph — the taxonomy names all four as what does NOT fit a millisecond, and this milestone's
// brief names it again. The cases that need any of those are `integration.vfx_compiler` and
// `integration.vfx`.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/vfx/budget.h>
#include <cy/vfx/events.h>
#include <cy/vfx/layout.h>

using namespace cy;
using namespace cy::vfx;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

[[nodiscard]] AttributeDecl declaration(f32 minimum, f32 maximum, f32 tolerance) noexcept {
    AttributeDecl decl;
    decl.name = Name::intern("attribute");
    decl.type = Name::intern("float");
    decl.minimum = minimum;
    decl.maximum = maximum;
    decl.tolerance = tolerance;
    return decl;
}

}  // namespace

// --- Precision selection: "provably sufficient" is a computation
// ----------------------------------

CY_TEST_CASE("a tolerance of zero — the default — never quantises anything") {
    // The safety property. An author who declares no tolerance gets `Float32`, whatever the range
    // looks like: a compiler that guessed from the range would quantise a normalised position.
    CY_CHECK_EQ(select_precision(declaration(0.0F, 1.0F, 0.0F), 1), Precision::Float32);
    CY_CHECK_EQ(select_precision(declaration(-1.0F, 1.0F, 0.0F), 3), Precision::Float32);
    CY_CHECK_EQ(select_precision(declaration(0.0F, 64.0F, 0.0F), 1), Precision::Float32);
}

CY_TEST_CASE("the smallest encoding whose step fits the declared tolerance is the one chosen") {
    // One part in 255 over [0, 1] is exactly what eight bits give.
    CY_CHECK_EQ(select_precision(declaration(0.0F, 1.0F, 1.0F / 255.0F), 4), Precision::Unorm8);
    // Ten times finer than that is not, and the next encoding that fits is the signed 16-bit one.
    CY_CHECK_EQ(select_precision(declaration(0.0F, 1.0F, 1.0F / 2000.0F), 4), Precision::Snorm16);
    // A range outside [-1, 1] rules out both fixed-point encodings whatever the tolerance says.
    CY_CHECK_EQ(select_precision(declaration(-64.0F, 64.0F, 0.5F), 3), Precision::Float16);
    // And a tolerance finer than a half float's step at that magnitude falls through to f32.
    CY_CHECK_EQ(select_precision(declaration(-64.0F, 64.0F, 0.001F), 3), Precision::Float32);
}

CY_TEST_CASE("an authoring override wins over the computation, which is what an override is") {
    AttributeDecl decl = declaration(-64.0F, 64.0F, 0.0F);
    decl.override_precision = Precision::Float16;
    CY_CHECK_EQ(select_precision(decl, 3), Precision::Float16);
}

CY_TEST_CASE(
    "an encoding that cannot represent the range reports a step of zero, not a small one") {
    // This is what makes `select_precision` reject rather than choose: a zero is "cannot", and a
    // comparison against the tolerance would otherwise accept the narrowest encoding every time.
    CY_CHECK_EQ(precision_step(Precision::Unorm8, -1.0F, 1.0F), 0.0F);
    CY_CHECK_EQ(precision_step(Precision::Snorm16, 0.0F, 4.0F), 0.0F);
    CY_CHECK_GT(precision_step(Precision::Unorm8, 0.0F, 1.0F), 0.0F);
}

// --- Storage at the chosen precision -------------------------------------------------------------

CY_TEST_CASE("a quantised attribute reads back quantised, which is what makes the layout real") {
    AttributeSlot slot;
    slot.name = Name::intern("color");
    slot.type = Float4;
    slot.components = 4;
    slot.precision = Precision::Unorm8;
    slot.stride = 4;
    slot.array_offset = 0;

    u8 storage[16] = {};
    const Span<u8> block(storage, sizeof(storage));
    store_component(block, slot, 0, 0, 0.5F);
    const f32 read = load_component(Span<const u8>(storage, sizeof(storage)), slot, 0, 0);
    // 0.5 is not representable in eight bits: 128/255 is the nearest, and the difference is the
    // quantisation the author asked for rather than a defect.
    CY_CHECK_NE(read, 0.5F);
    CY_CHECK_NEAR(read, 0.5F, 1.0F / 255.0F);

    slot.precision = Precision::Float32;
    slot.stride = 16;
    store_component(block, slot, 0, 0, 0.5F);
    CY_CHECK_EQ(load_component(Span<const u8>(storage, sizeof(storage)), slot, 0, 0), 0.5F);
}

CY_TEST_CASE("a write past the end of a block is ignored rather than trapping") {
    AttributeSlot slot;
    slot.name = Name::intern("position");
    slot.components = 3;
    slot.precision = Precision::Float32;
    slot.array_offset = 0;
    u8 storage[12] = {};
    const Span<u8> block(storage, sizeof(storage));
    store_component(block, slot, 99, 0, 1.0F);
    CY_CHECK_EQ(load_component(Span<const u8>(storage, sizeof(storage)), slot, 99, 0), 0.0F);
}

// --- The budget controller
// ------------------------------------------------------------------------

CY_TEST_CASE("VFX does not pay for another subsystem's cost") {
    // `vfx-system`: "WHEN the frame is slow because of geometry cost while VFX is within its
    // allocation THEN the VFX controller SHALL make no adjustment." There is no frame time in this
    // interface, so the only way to say "the frame is slow" is not to say it — and the controller
    // holds every lever where it was.
    BudgetController controller;
    controller.set_allocation(2.0F);
    controller.measure(1.2F);
    for (u32 frame = 0; frame < 60U; ++frame) {
        controller.update(1.0F / 60.0F);
    }
    CY_CHECK_EQ(controller.state().quality[0], 1.0F);
    CY_CHECK_EQ(controller.state().quality[3], 1.0F);
    CY_CHECK_FALSE(controller.state().over_budget);
}

CY_TEST_CASE("over budget, the least important class is reduced first and Critical last") {
    BudgetController controller;
    controller.set_allocation(2.0F);
    controller.measure(3.4F);  // `vfx-system`'s own "Battle exceeds the budget" numbers.
    for (u32 frame = 0; frame < 30U; ++frame) {
        controller.update(1.0F / 60.0F);
    }
    const BudgetState& state = controller.state();
    CY_CHECK(state.over_budget);
    CY_CHECK_LT(state.quality[static_cast<u32>(ImportanceClass::Decorative)], 1.0F);
    CY_CHECK_EQ(state.quality[static_cast<u32>(ImportanceClass::Critical)], 1.0F);
    CY_CHECK_EQ(state.lowest_reduced, ImportanceClass::Decorative);
    CY_CHECK_GT(controller.applied().count(), 0U);
}

CY_TEST_CASE("Critical survives at its floor, and the reserved minimum is reported when reached") {
    BudgetController controller;
    controller.set_allocation(0.5F);
    controller.measure(40.0F);
    for (u32 frame = 0; frame < 600U; ++frame) {
        controller.update(1.0F / 60.0F);
    }
    const BudgetState& state = controller.state();
    // "Critical effects SHALL still render at their configured minimum quality."
    CY_CHECK_EQ(state.quality[static_cast<u32>(ImportanceClass::Critical)], kCriticalFloor);
    CY_CHECK_EQ(state.quality[static_cast<u32>(ImportanceClass::Decorative)], 0.0F);
    // "SHALL report when it has reached [the reserved minimum] so the arbiter can reallocate rather
    // than continue reducing a subsystem with nothing left to give."
    CY_CHECK(state.at_reserved_minimum);
    CY_CHECK_GT(controller.levers(ImportanceClass::Critical).spawn_scale, 0.0F);
}

CY_TEST_CASE("hysteresis: a measurement between the two thresholds moves nothing") {
    BudgetController controller;
    controller.set_allocation(2.0F);
    controller.measure(3.4F);
    for (u32 frame = 0; frame < 20U; ++frame) {
        controller.update(1.0F / 60.0F);
    }
    const f32 settled = controller.state().quality[static_cast<u32>(ImportanceClass::Decorative)];
    CY_REQUIRE(settled < 1.0F);

    // Just inside the dead band on both sides. Neither reduces nor restores.
    controller.measure(2.0F * 1.02F);
    controller.update(1.0F / 60.0F);
    CY_CHECK_EQ(controller.state().quality[static_cast<u32>(ImportanceClass::Decorative)], settled);
    controller.measure(2.0F * 0.9F);
    controller.update(1.0F / 60.0F);
    CY_CHECK_EQ(controller.state().quality[static_cast<u32>(ImportanceClass::Decorative)], settled);

    // Below the restore threshold it comes back — most important first.
    controller.measure(0.4F);
    controller.update(1.0F / 60.0F);
    CY_CHECK_GT(controller.state().quality[static_cast<u32>(ImportanceClass::Decorative)], settled);
}

CY_TEST_CASE("pinned mode reports the excess and corrects nothing") {
    BudgetController controller;
    controller.set_allocation(2.0F);
    controller.set_pinned(true);
    controller.measure(9.0F);
    for (u32 frame = 0; frame < 120U; ++frame) {
        controller.update(1.0F / 60.0F);
    }
    CY_CHECK(controller.state().over_budget);
    for (const f32 quality : controller.state().quality) {
        CY_CHECK_EQ(quality, 1.0F);
    }
    CY_CHECK_EQ(controller.applied().count(), 0U);
}

CY_TEST_CASE("an effect's own scalability floors override the controller's levers") {
    BudgetController controller;
    controller.set_allocation(0.5F);
    controller.measure(40.0F);
    for (u32 frame = 0; frame < 600U; ++frame) {
        controller.update(1.0F / 60.0F);
    }
    ScalabilityPolicy policy;
    policy.min_spawn_scale = 0.8F;
    policy.min_simulation_hz = 45.0F;
    policy.may_drop_sorting = false;
    const BudgetLevers levers = controller.levers_for(ImportanceClass::Decorative, policy);
    CY_CHECK_GE(levers.spawn_scale, 0.8F);
    CY_CHECK_GE(levers.simulation_hz, 45.0F);
    CY_CHECK(levers.sorted);
}

// --- Event channels: both bounds, and the deterministic rank
// ---------------------------------------

namespace {

[[nodiscard]] EventChannelDecl channel(u32 per_frame, u32 depth) noexcept {
    EventChannelDecl decl;
    decl.name = Name::intern("collision");
    decl.max_events_per_frame = per_frame;
    decl.max_chain_depth = depth;
    return decl;
}

[[nodiscard]] EventRecord event(f32 rank, u32 depth = 0) noexcept {
    EventRecord record;
    record.rank = rank;
    record.depth = depth;
    return record;
}

}  // namespace

CY_TEST_CASE("a channel cannot be declared without both of its bounds") {
    EventRouter router(allocator());
    CY_CHECK_FALSE(router.declare(channel(0, 4)).has_value());
    CY_CHECK_FALSE(router.declare(channel(64, 0)).has_value());
    CY_CHECK(router.declare(channel(64, 4)).has_value());
}

CY_TEST_CASE("a full channel keeps the highest-ranked events and reports the overflow") {
    EventRouter router(allocator());
    CY_REQUIRE(router.declare(channel(4, 8)).has_value());
    router.begin_frame();
    // Ten events, ranks 0..9, offered in ascending order. The four that survive are the four
    // highest, whatever order they arrived in — that is the deterministic rank.
    for (u32 which = 0; which < 10U; ++which) {
        CY_REQUIRE(
            router.raise(Name::intern("collision"), event(static_cast<f32>(which))).has_value());
    }
    const Span<const EventRecord> live = router.consume(Name::intern("collision"));
    CY_CHECK_EQ(live.size(), 4U);
    f32 lowest = 1000.0F;
    for (const EventRecord& record : live) {
        lowest = record.rank < lowest ? record.rank : lowest;
    }
    CY_CHECK_EQ(lowest, 6.0F);
    CY_CHECK_EQ(router.total_dropped(), 6U);
    CY_CHECK_EQ(router.report(Name::intern("collision"))->raised, 10U);
}

CY_TEST_CASE("the surviving set does not depend on arrival order") {
    EventRouter ascending(allocator());
    EventRouter descending(allocator());
    CY_REQUIRE(ascending.declare(channel(3, 8)).has_value());
    CY_REQUIRE(descending.declare(channel(3, 8)).has_value());
    ascending.begin_frame();
    descending.begin_frame();
    for (u32 which = 0; which < 8U; ++which) {
        CY_REQUIRE(
            ascending.raise(Name::intern("collision"), event(static_cast<f32>(which))).has_value());
        CY_REQUIRE(descending.raise(Name::intern("collision"), event(static_cast<f32>(7U - which)))
                       .has_value());
    }
    f32 ascending_sum = 0.0F;
    f32 descending_sum = 0.0F;
    for (const EventRecord& record : ascending.consume(Name::intern("collision"))) {
        ascending_sum += record.rank;
    }
    for (const EventRecord& record : descending.consume(Name::intern("collision"))) {
        descending_sum += record.rank;
    }
    CY_CHECK_EQ(ascending_sum, descending_sum);
    CY_CHECK_EQ(ascending_sum, 5.0F + 6.0F + 7.0F);
}

CY_TEST_CASE("a chain terminates at its declared depth and the truncation is reported") {
    EventRouter router(allocator());
    CY_REQUIRE(router.declare(channel(64, 3)).has_value());
    router.begin_frame();
    // A feedback loop: each event spawns the next one deeper. Without the bound this diverges.
    u32 depth = 0;
    u32 raised = 0;
    while (depth < 16U) {
        CY_REQUIRE(router.raise(Name::intern("collision"), event(1.0F, depth)).has_value());
        ++raised;
        ++depth;
    }
    CY_CHECK_EQ(router.consume(Name::intern("collision")).size(), 3U);
    CY_CHECK_EQ(router.total_truncated(), raised - 3U);
    CY_CHECK_EQ(router.total_dropped(), 0U);
}

CY_TEST_CASE("raising on an undeclared channel is refused rather than creating one") {
    EventRouter router(allocator());
    router.begin_frame();
    CY_CHECK_FALSE(router.raise(Name::intern("nothing"), event(1.0F)).has_value());
}
