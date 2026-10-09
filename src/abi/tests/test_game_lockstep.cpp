// SPDX-License-Identifier: MIT
// ABI 1.8's lockstep thunks against a fake backend. openspec/changes/add-deterministic-math
// stage 8.
//
// What the thunks own: the phases — enlisting `[N]`, ordering `[F U]`, reading anywhere — the
// argument checks, `struct_size` both ways, and UNAVAILABLE without a session. The session itself
// is integration.game_backend_lockstep's.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/lockstep.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstddef>

namespace {

using cy::u32;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

class FakeLockstep final : public cy::abi::game::LockstepBackend {
public:
    u32 enlisted = 0;
    u32 orders = 0;
    CyLockstepUnitDesc last_desc{};
    CyLockstepOrder last_order{};

    CyResult enlist(const CyLockstepUnitDesc& desc, u32& out_unit) noexcept override {
        last_desc = desc;
        out_unit = enlisted++;
        return CY_RESULT_OK;
    }
    CyResult order(const CyLockstepOrder& order) noexcept override {
        last_order = order;
        ++orders;
        return CY_RESULT_OK;
    }
    CyResult unit(u32 index, CyLockstepUnit& out) const noexcept override {
        if (index >= enlisted) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no such unit");
        }
        out.group = 3;
        out.position = CyFixedVec2{1, 2};
        out.flags = CY_LOCKSTEP_UNIT_ARRIVED;
        return CY_RESULT_OK;
    }
    CyResult status(CyLockstepStatus& out) const noexcept override {
        out.units = enlisted;
        out.tick = 9;
        out.digest = 0xD16E57ULL;
        return CY_RESULT_OK;
    }
};

struct Fixture {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    FakeLockstep lockstep;
    Fixture() noexcept { host.game.lockstep = &lockstep; }
    [[nodiscard]] CyEngine engine() noexcept { return &host; }
};

[[nodiscard]] CyLockstepUnitDesc desc() noexcept {
    CyLockstepUnitDesc unit{};
    unit.struct_size = sizeof(unit);
    unit.group = 3;
    unit.position = CyFixedVec2{4, 5};
    return unit;
}

[[nodiscard]] CyLockstepOrder move() noexcept {
    CyLockstepOrder order{};
    order.struct_size = sizeof(order);
    order.kind = CY_LOCKSTEP_ORDER_MOVE;
    order.group = 3;
    order.target = CyFixedVec2{6, 7};
    return order;
}

}  // namespace

CY_TEST_CASE("lockstep: enlisting is configuration, orders are a step's or a frame's") {
    Fixture fixture;
    const CyInterface& iface = table();
    const CyLockstepUnitDesc unit = desc();
    const CyLockstepOrder order = move();
    u32 index = 99;
    CyLockstepStatus status{};
    status.struct_size = sizeof(status);

    // No phase: enlist, and no orders.
    CY_CHECK_EQ(iface.lockstep_enlist(fixture.engine(), &unit, &index), CY_RESULT_OK);
    CY_CHECK_EQ(index, 0U);
    CY_CHECK_EQ(fixture.lockstep.last_desc.position.y, 5);
    CY_CHECK_EQ(iface.lockstep_order(fixture.engine(), &order), CY_RESULT_PERMISSION_DENIED);
    CY_CHECK_EQ(iface.lockstep_status(fixture.engine(), &status), CY_RESULT_OK);
    for (const CyPhase phase : {CY_PHASE_FIXED_UPDATE, CY_PHASE_FRAME_UPDATE}) {
        const cy::abi::game::PhaseScope scope(fixture.host.game.clock, phase);
        CY_CHECK_EQ(iface.lockstep_enlist(fixture.engine(), &unit, &index),
                    CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(iface.lockstep_order(fixture.engine(), &order), CY_RESULT_OK);
        CY_CHECK_EQ(iface.lockstep_status(fixture.engine(), &status), CY_RESULT_OK);
    }
    CY_CHECK_EQ(fixture.lockstep.enlisted, 1U);
    CY_CHECK_EQ(fixture.lockstep.orders, 2U);
    CY_CHECK_EQ(fixture.lockstep.last_order.target.x, 6);
    CY_CHECK_EQ(status.tick, 9U);
    CY_CHECK_EQ(status.digest, 0xD16E57ULL);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("lockstep: the arguments a thunk refuses before a backend sees them") {
    Fixture fixture;
    const CyInterface& iface = table();
    CyLockstepUnitDesc unit = desc();
    u32 index = 0;
    CY_CHECK_EQ(iface.lockstep_enlist(nullptr, &unit, &index), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.lockstep_enlist(fixture.engine(), nullptr, &index),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.lockstep_enlist(fixture.engine(), &unit, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    unit.radius = -1;
    CY_CHECK_EQ(iface.lockstep_enlist(fixture.engine(), &unit, &index), CY_RESULT_INVALID_ARGUMENT);
    unit = desc();
    unit.struct_size = 2;
    CY_CHECK_EQ(iface.lockstep_enlist(fixture.engine(), &unit, &index), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(fixture.lockstep.enlisted, 0U);

    const cy::abi::game::PhaseScope fixed(fixture.host.game.clock, CY_PHASE_FIXED_UPDATE);
    CyLockstepOrder order = move();
    order.kind = 2;
    CY_CHECK_EQ(iface.lockstep_order(fixture.engine(), &order), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.lockstep_order(fixture.engine(), nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(fixture.lockstep.orders, 0U);
    CY_CHECK_EQ(iface.lockstep_unit(fixture.engine(), 0, nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.lockstep_status(fixture.engine(), nullptr), CY_RESULT_INVALID_ARGUMENT);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("lockstep: struct_size both ways, and the backend's NOT_FOUND passes through") {
    Fixture fixture;
    const CyInterface& iface = table();
    const CyLockstepUnitDesc unit = desc();
    u32 index = 0;
    CY_REQUIRE_EQ(iface.lockstep_enlist(fixture.engine(), &unit, &index), CY_RESULT_OK);
    // A caller compiled against a shorter struct: only its prefix is written, and its size says so.
    CyLockstepUnit shorter{};
    shorter.struct_size = static_cast<u32>(offsetof(CyLockstepUnit, velocity));
    shorter.flags = 0xABCDU;
    CY_CHECK_EQ(iface.lockstep_unit(fixture.engine(), 0, &shorter), CY_RESULT_OK);
    CY_CHECK_EQ(shorter.struct_size, static_cast<u32>(offsetof(CyLockstepUnit, velocity)));
    CY_CHECK_EQ(shorter.group, 3U);
    CY_CHECK_EQ(shorter.position.y, 2);
    CY_CHECK_EQ(shorter.flags, 0xABCDU);
    CyLockstepUnit whole{};
    whole.struct_size = sizeof(whole);
    CY_CHECK_EQ(iface.lockstep_unit(fixture.engine(), 0, &whole), CY_RESULT_OK);
    CY_CHECK_EQ(whole.flags, CY_LOCKSTEP_UNIT_ARRIVED);
    CY_CHECK_EQ(iface.lockstep_unit(fixture.engine(), 5, &whole), CY_RESULT_NOT_FOUND);
    whole.struct_size = 2;
    CY_CHECK_EQ(iface.lockstep_unit(fixture.engine(), 0, &whole), CY_RESULT_INVALID_ARGUMENT);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("lockstep: without a session every entry is UNAVAILABLE") {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    const CyInterface& iface = table();
    const CyLockstepUnitDesc unit = desc();
    u32 index = 0;
    CY_CHECK_EQ(iface.lockstep_enlist(&host, &unit, &index), CY_RESULT_UNAVAILABLE);
    CyLockstepStatus status{};
    status.struct_size = sizeof(status);
    CY_CHECK_EQ(iface.lockstep_status(&host, &status), CY_RESULT_UNAVAILABLE);
    cy::abi::clear_last_error();
}
