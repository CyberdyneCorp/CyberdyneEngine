// SPDX-License-Identifier: MIT
// ABI 1.3's `time_get`, and the phase the behaviour runtime dispatches in. `add-swift-game-api`.
//
// OWNER: implementer C. `time_get` has no backend — it reads the host clock — so these cases need
// no fake; the runtime cases register a C vtable directly, with no module image, and let each
// callback ask `time_get` what it sees.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/abi/module.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstddef>
#include <cstring>

namespace {

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Scripting);
}

/// A host whose clock holds a frame's worth of everything, so a fixed step's zeroes are visible.
struct ClockHost {
    cy::abi::Host host{allocator()};

    ClockHost() {
        host.game.clock.tick = 1234;
        host.game.clock.fixed_delta = 1.0 / 30.0;
        host.game.clock.frame_delta = 0.016;
        host.game.clock.interpolation = 0.25;
        host.game.clock.flags = CY_TIME_PAUSED;
    }
};

CyTime whole_time() noexcept {
    CyTime time{};
    time.struct_size = sizeof(CyTime);
    return time;
}

// --- A behaviour that records what `time_get` said from inside each callback ---------------------

struct Seen {
    CyTime fixed{};
    CyTime frame{};
    int fixed_calls = 0;
    int frame_calls = 0;
};

Seen g_seen;
CyEngine g_engine = nullptr;

CyInstance probe_create(CyEngine engine, CyEntity /*entity*/, void* /*user_data*/) {
    g_engine = engine;
    return reinterpret_cast<CyInstance>(&g_seen);
}
void probe_destroy(CyInstance /*self*/, void* /*user_data*/) {}
void probe_fixed(CyInstance /*self*/, float /*dt*/, void* /*user_data*/) {
    g_seen.fixed = whole_time();
    CY_CHECK_EQ(table().time_get(g_engine, &g_seen.fixed), CY_RESULT_OK);
    ++g_seen.fixed_calls;
}
void probe_frame(CyInstance /*self*/, float /*dt*/, void* /*user_data*/) {
    g_seen.frame = whole_time();
    CY_CHECK_EQ(table().time_get(g_engine, &g_seen.frame), CY_RESULT_OK);
    ++g_seen.frame_calls;
}

CyBehaviourVTable probe_vtable(bool with_frame) noexcept {
    CyBehaviourVTable vtable{};
    vtable.struct_size = sizeof(CyBehaviourVTable);
    vtable.create = &probe_create;
    vtable.destroy = &probe_destroy;
    vtable.fixed_update = &probe_fixed;
    vtable.frame_update = with_frame ? &probe_frame : nullptr;
    return vtable;
}

}  // namespace

CY_TEST_CASE("time_get reports the clock, and never refuses a phase") {
    ClockHost fixture;
    for (const CyPhase phase : {CY_PHASE_NONE, CY_PHASE_FIXED_UPDATE, CY_PHASE_FRAME_UPDATE}) {
        fixture.host.game.clock.phase = phase;
        CyTime time = whole_time();
        CY_CHECK_EQ(table().time_get(&fixture.host, &time), CY_RESULT_OK);
        CY_CHECK_EQ(time.phase, static_cast<uint32_t>(phase));
        CY_CHECK_EQ(time.tick, 1234U);
        CY_CHECK_EQ(time.fixed_delta, 1.0 / 30.0);
        CY_CHECK_EQ(time.flags, CY_TIME_PAUSED);
        CY_CHECK_EQ(time.struct_size, sizeof(CyTime));
    }
}

CY_TEST_CASE("in a fixed step time_get hides the frame delta and the interpolation") {
    ClockHost fixture;
    fixture.host.game.clock.phase = CY_PHASE_FRAME_UPDATE;
    CyTime frame = whole_time();
    CY_REQUIRE_EQ(table().time_get(&fixture.host, &frame), CY_RESULT_OK);
    CY_CHECK_EQ(frame.frame_delta, 0.016);
    CY_CHECK_EQ(frame.interpolation, 0.25);

    fixture.host.game.clock.phase = CY_PHASE_FIXED_UPDATE;
    CyTime fixed = whole_time();
    CY_REQUIRE_EQ(table().time_get(&fixture.host, &fixed), CY_RESULT_OK);
    CY_CHECK_EQ(fixed.frame_delta, 0.0);
    CY_CHECK_EQ(fixed.interpolation, 0.0);
}

CY_TEST_CASE("time_get refuses a null engine, a null output and a malformed struct_size") {
    ClockHost fixture;
    CyTime time = whole_time();
    CY_CHECK_EQ(table().time_get(nullptr, &time), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().time_get(&fixture.host, nullptr), CY_RESULT_INVALID_ARGUMENT);

    std::memset(&time, 0xEE, sizeof(time));
    time.struct_size = 2;
    CY_CHECK_EQ(table().time_get(&fixture.host, &time), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(time.tick, 0xEEEEEEEEEEEEEEEEULL);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("an older caller's short CyTime is written only as far as it reaches") {
    ClockHost fixture;
    CyTime time{};
    std::memset(&time, 0xEE, sizeof(time));
    const auto prefix = static_cast<uint32_t>(offsetof(CyTime, fixed_delta));
    time.struct_size = prefix;
    CY_REQUIRE_EQ(table().time_get(&fixture.host, &time), CY_RESULT_OK);
    CY_CHECK_EQ(time.struct_size, prefix);
    CY_CHECK_EQ(time.tick, 1234U);
    CY_CHECK_EQ(time.flags, 0xEEEEEEEEU);

    // Zero is "this header's size".
    CyTime zero{};
    CY_REQUIRE_EQ(table().time_get(&fixture.host, &zero), CY_RESULT_OK);
    CY_CHECK_EQ(zero.struct_size, sizeof(CyTime));
    CY_CHECK_EQ(zero.flags, CY_TIME_PAUSED);
}

CY_TEST_CASE("the runtime dispatches fixed_update in F and frame_update in U, then restores N") {
    ClockHost fixture;
    g_seen = Seen{};
    {
        cy::abi::BehaviourRuntime runtime(allocator(), fixture.host);
        CY_REQUIRE(fixture.host.register_behaviour("TimeProbe", probe_vtable(true)).has_value());
        CY_REQUIRE(runtime.create("TimeProbe", CY_ENTITY_NULL).has_value());

        runtime.fixed_update(0.02F);
        CY_CHECK_EQ(fixture.host.game.clock.phase, CY_PHASE_NONE);
        CY_REQUIRE_EQ(g_seen.fixed_calls, 1);
        CY_CHECK_EQ(g_seen.fixed.phase, static_cast<uint32_t>(CY_PHASE_FIXED_UPDATE));
        CY_CHECK_EQ(g_seen.fixed.fixed_delta, static_cast<double>(0.02F));
        // F hides the frame delta even though the clock holds one.
        CY_CHECK_EQ(g_seen.fixed.frame_delta, 0.0);

        runtime.frame_update(0.005F);
        CY_CHECK_EQ(fixture.host.game.clock.phase, CY_PHASE_NONE);
        CY_REQUIRE_EQ(g_seen.frame_calls, 1);
        CY_CHECK_EQ(g_seen.frame.phase, static_cast<uint32_t>(CY_PHASE_FRAME_UPDATE));
        CY_CHECK_EQ(g_seen.frame.frame_delta, static_cast<double>(0.005F));
    }
}

CY_TEST_CASE("a behaviour with no frame callback is never scheduled for a frame") {
    ClockHost fixture;
    g_seen = Seen{};
    cy::abi::BehaviourRuntime runtime(allocator(), fixture.host);
    // A module compiled before 1.3: its vtable ends at `user_data`, so `frame_update` is not
    // copied.
    CyBehaviourVTable old = probe_vtable(true);
    old.struct_size = static_cast<uint32_t>(offsetof(CyBehaviourVTable, frame_update));
    CY_REQUIRE(fixture.host.register_behaviour("OldProbe", old).has_value());
    CY_REQUIRE(runtime.create("OldProbe", CY_ENTITY_NULL).has_value());

    runtime.frame_update(0.016F);
    CY_CHECK_EQ(g_seen.frame_calls, 0);
    runtime.fixed_update(0.02F);
    CY_CHECK_EQ(g_seen.fixed_calls, 1);
}
