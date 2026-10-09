// SPDX-License-Identifier: MIT
// `integration.game_backend_lockstep`: the lockstep session and its adapter, through ABI 1.8's
// table. openspec/changes/add-deterministic-math stage 8.
//
// A real session: the RTS map of src/movement/tests/rts_scenario.h converted into a Fixed world,
// units in two groups, orders through `lockstep_order`, a follower peer driven by the issuer's log
// alone. What the thunks refuse without a session is unit.abi's (test_game_lockstep.cpp).

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/lockstep_backend.h>
#include <cy/test/test.h>

#include <memory>

#include "rts_scenario.h"

using namespace cy;
using cy::game_backend::LockstepAdapter;
using cy::game_backend::LockstepConfig;
using cy::game_backend::LockstepSession;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

constexpr u32 kMapCells = 32;
constexpr i64 kOne = detmath::Fixed::kOneRaw;

/// Two peers of one session behind the ABI: the issuer the adapter answers from, and a follower.
struct Peers {
    navigation::NavMesh baked{allocator(), Name::intern("lockstep.map"), 64.0F};
    std::unique_ptr<LockstepSession> issuer;
    std::unique_ptr<LockstepSession> follower;
    std::unique_ptr<LockstepAdapter> adapter;
    abi::Host host{system_allocator(MemoryDomain::Scripting)};

    explicit Peers(const LockstepConfig& config, bool with_follower = true) noexcept {
        CY_REQUIRE(movement_test::bake_rts_mesh(kMapCells, baked));
        issuer = std::make_unique<LockstepSession>(allocator(), config);
        CY_REQUIRE(issuer->load(baked).has_value());
        if (with_follower) {
            follower = std::make_unique<LockstepSession>(allocator(), config);
            CY_REQUIRE(follower->load(baked).has_value());
        }
        adapter = std::make_unique<LockstepAdapter>(*issuer, follower.get());
        game_backend::bind_lockstep(host, adapter.get());
    }

    [[nodiscard]] CyEngine engine() noexcept { return &host; }
};

/// `count` units of `group` in a row along x from (`x`, `z`) metres, through the ABI.
void enlist_row(Peers& peers, u32 group, i32 x, i32 z, u32 count) noexcept {
    for (u32 index = 0; index < count; ++index) {
        CyLockstepUnitDesc desc{};
        desc.struct_size = sizeof(desc);
        desc.group = group;
        desc.entity = 100 + index;
        desc.position = CyFixedVec2{(static_cast<i64>(x) * kOne) + (static_cast<i64>(index) * kOne),
                                    static_cast<i64>(z) * kOne};
        u32 unit = 0;
        CY_REQUIRE_EQ(table().lockstep_enlist(peers.engine(), &desc, &unit), CY_RESULT_OK);
    }
}

void order(Peers& peers, u32 group, i32 x, i32 z) noexcept {
    CyLockstepOrder move{};
    move.struct_size = sizeof(move);
    move.kind = CY_LOCKSTEP_ORDER_MOVE;
    move.group = group;
    move.target = CyFixedVec2{static_cast<i64>(x) * kOne, static_cast<i64>(z) * kOne};
    const abi::game::PhaseScope fixed(peers.host.game.clock, CY_PHASE_FIXED_UPDATE);
    CY_REQUIRE_EQ(table().lockstep_order(peers.engine(), &move), CY_RESULT_OK);
}

/// The scenario: two groups of nine meet in the middle of the map, then the first goes home.
[[nodiscard]] u32 run_crossing(Peers& peers, u32 ticks, u32* arrivals = nullptr) noexcept {
    enlist_row(peers, 0, 4, 6, 9);
    enlist_row(peers, 1, 46, 52, 9);
    u32 arrived = 0;
    for (u32 tick = 0; tick < ticks; ++tick) {
        if (tick == 0) {
            order(peers, 0, 22, 14);
            order(peers, 1, 26, 46);
        }
        if (tick == 600) {
            order(peers, 0, 8, 10);
        }
        CY_REQUIRE(peers.adapter->tick().has_value());
        for (u32 unit = 0; unit < peers.issuer->units(); ++unit) {
            CyLockstepUnit state{};
            state.struct_size = sizeof(state);
            CY_REQUIRE_EQ(table().lockstep_unit(peers.engine(), unit, &state), CY_RESULT_OK);
            arrived += (state.flags & CY_LOCKSTEP_UNIT_ARRIVED) != 0U ? 1U : 0U;
        }
    }
    if (arrivals != nullptr) {
        *arrivals = arrived;
    }
    return peers.adapter->disagreements();
}

}  // namespace

CY_TEST_CASE("lockstep: two peers driven by orders through the ABI agree on every tick") {
    Peers peers(LockstepConfig{});
    u32 arrivals = 0;
    const u32 disagreements = run_crossing(peers, 1000, &arrivals);
    CY_CHECK_EQ(disagreements, 0U);
    CY_CHECK_EQ(peers.issuer->digest(), peers.follower->digest());
    CY_CHECK_EQ(peers.issuer->orders_executed(), 3U);
    CY_CHECK_EQ(peers.follower->orders_executed(), 3U);
    CY_CHECK_EQ(peers.issuer->paths_found(), peers.issuer->paths_planned());
    // Every unit of both groups reached its slot, and group 0 twice.
    CY_CHECK_EQ(arrivals, 27U);

    CyLockstepStatus status{};
    status.struct_size = sizeof(status);
    CY_REQUIRE_EQ(table().lockstep_status(peers.engine(), &status), CY_RESULT_OK);
    CY_CHECK_EQ(status.units, 18U);
    CY_CHECK_EQ(status.tick, 1000U);
    CY_CHECK_EQ(status.commands, 3U);
    CY_CHECK_EQ(status.digest, peers.issuer->digest());
    CY_CHECK_EQ(status.disagreements, 0U);
    CY_CHECK_EQ(status.kernel_version, table().detmath_kernel_version());
    // The session passed the Lockstep profile check with its three authoritative subsystems.
    CY_CHECK_EQ(peers.issuer->admitted_subsystems(), 3U);
}

CY_TEST_CASE("lockstep: a follower that misses one order diverges, and the adapter counts it") {
    // The negative control: the follower runs the same session from a log missing the last order.
    Peers peers(LockstepConfig{}, false);
    LockstepSession follower(allocator(), LockstepConfig{});
    CY_REQUIRE(follower.load(peers.baked).has_value());
    enlist_row(peers, 0, 4, 6, 9);
    enlist_row(peers, 1, 46, 52, 9);
    for (u32 unit = 0; unit < peers.issuer->units(); ++unit) {
        game_backend::LockstepUnitSpec spec;
        auto state = peers.issuer->unit(unit);
        CY_REQUIRE(state.has_value());
        spec.group = state->group;
        spec.position = state->position;
        CY_REQUIRE(follower.enlist(spec).has_value());
    }
    u32 cursor = 0;
    u32 differing = 0;
    for (u32 tick = 0; tick < 200; ++tick) {
        if (tick == 0) {
            order(peers, 0, 50, 54);
        }
        if (tick == 100) {
            order(peers, 1, 10, 8);
        }
        CY_REQUIRE(peers.adapter->tick().has_value());
        if (tick != 100) {
            CY_REQUIRE(follower.receive(peers.issuer->log(), cursor).has_value());
        } else {
            ++cursor;  // the dropped order
        }
        CY_REQUIRE(follower.advance().has_value());
        differing += follower.state_hash() == peers.issuer->state_hash() ? 0U : 1U;
    }
    CY_CHECK_EQ(differing, 100U);
}

CY_TEST_CASE("lockstep: avoidance is part of the session, and of its digest") {
    LockstepConfig without;
    without.avoidance = false;
    Peers avoiding(LockstepConfig{});
    Peers separating(without);
    CY_CHECK_EQ(run_crossing(avoiding, 300), 0U);
    CY_CHECK_EQ(run_crossing(separating, 300), 0U);
    CY_CHECK_NE(avoiding.issuer->world_hash(), separating.issuer->world_hash());
    CY_CHECK_NE(avoiding.issuer->digest(), separating.issuer->digest());
}

CY_TEST_CASE("lockstep: what the session refuses, through the ABI") {
    Peers peers(LockstepConfig{});
    enlist_row(peers, 0, 4, 6, 2);
    CyLockstepOrder move{};
    move.struct_size = sizeof(move);
    move.kind = CY_LOCKSTEP_ORDER_MOVE;
    move.group = 7;
    {
        const abi::game::PhaseScope fixed(peers.host.game.clock, CY_PHASE_FIXED_UPDATE);
        // A group no unit is in.
        CY_CHECK_EQ(table().lockstep_order(peers.engine(), &move), CY_RESULT_NOT_FOUND);
    }
    CY_REQUIRE(peers.adapter->tick().has_value());
    // Enlisting after the first tick: the session has started.
    CyLockstepUnitDesc late{};
    late.struct_size = sizeof(late);
    u32 unit = 0;
    CY_CHECK_EQ(table().lockstep_enlist(peers.engine(), &late, &unit), CY_RESULT_PERMISSION_DENIED);
    // An index never enlisted.
    CyLockstepUnit state{};
    state.struct_size = sizeof(state);
    CY_CHECK_EQ(table().lockstep_unit(peers.engine(), 2, &state), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().lockstep_unit(peers.engine(), 1, &state), CY_RESULT_OK);
    CY_CHECK_EQ(state.group, 0U);
    CY_CHECK_EQ(state.entity, 101U);
    abi::clear_last_error();
}
