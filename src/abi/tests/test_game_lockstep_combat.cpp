// SPDX-License-Identifier: MIT
// ABI 1.16's lockstep combat thunks against a fake backend. openspec/changes/
// add-rts-combat-and-minimap.
//
// What the thunks own: arming and fortifying are configuration `[N]`, reading answers anywhere;
// the argument checks — a weapon that is a CyLockstepWeapon, a zero reserved word, `struct_size`
// both ways, the sizing pattern of the two lists — and a backend with no combat answering
// UNSUPPORTED. The battle itself is integration.game_backend_lockstep's.

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
using cy::u64;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

/// A session with no combat: only the 1.8 entries overridden.
class PeacefulLockstep : public cy::abi::game::LockstepBackend {
public:
    CyResult enlist(const CyLockstepUnitDesc& /*desc*/, u32& out_unit) noexcept override {
        out_unit = 0;
        return CY_RESULT_OK;
    }
    CyResult order(const CyLockstepOrder& /*order*/) noexcept override { return CY_RESULT_OK; }
    CyResult unit(u32 /*index*/, CyLockstepUnit& /*out*/) const noexcept override {
        return CY_RESULT_OK;
    }
    CyResult status(CyLockstepStatus& /*out*/) const noexcept override { return CY_RESULT_OK; }
};

/// Three events numbered 10 to 12 and one projectile; a unit 0 armed by the last `arm`.
class FakeCombat final : public PeacefulLockstep {
public:
    u32 armed = 0;
    u32 fortified = 0;
    CyLockstepCombatDesc last_arm{};
    CyLockstepStructureDesc last_fortify{};

    CyResult arm(u32 unit, const CyLockstepCombatDesc& desc) noexcept override {
        if (unit != 0U) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no such unit");
        }
        last_arm = desc;
        ++armed;
        return CY_RESULT_OK;
    }
    CyResult fortify(const CyLockstepStructureDesc& desc, u32& out) noexcept override {
        last_fortify = desc;
        out = fortified++;
        return CY_RESULT_OK;
    }
    CyResult combatant(u32 unit, CyLockstepCombatant& out) const noexcept override {
        if (unit != 0U) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no such unit");
        }
        out.flags = CY_LOCKSTEP_COMBATANT_ARMED | CY_LOCKSTEP_COMBATANT_ALIVE;
        out.health = 70;
        out.max_health = 100;
        out.target_kind = CY_LOCKSTEP_TARGET_STRUCTURE;
        out.target = 2;
        return CY_RESULT_OK;
    }
    CyResult structure(u32 index, CyLockstepStructure& out) const noexcept override {
        if (index >= fortified) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no such structure");
        }
        out.health = 450;
        out.max_health = 500;
        return CY_RESULT_OK;
    }
    CyResult events(u64 first, CyLockstepEvent* out, u32 capacity,
                    u32& out_count) const noexcept override {
        u32 count = 0;
        for (u64 sequence = first < 10U ? 10U : first; sequence <= 12U; ++sequence) {
            if (out != nullptr && count < capacity) {
                out[count] = CyLockstepEvent{};
                out[count].sequence = sequence;
                out[count].kind = CY_LOCKSTEP_EVENT_DAMAGE;
            }
            ++count;
        }
        out_count = out == nullptr ? count : (count < capacity ? count : capacity);
        return CY_RESULT_OK;
    }
    CyResult projectiles(CyLockstepProjectile* out, u32 capacity,
                         u32& out_count) const noexcept override {
        if (out != nullptr && capacity > 0U) {
            out[0] = CyLockstepProjectile{};
            out[0].id = 7;
        }
        out_count = out == nullptr || capacity > 0U ? 1U : 0U;
        return CY_RESULT_OK;
    }
};

template <class Backend>
struct Fixture {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    Backend lockstep;
    Fixture() noexcept { host.game.lockstep = &lockstep; }
    [[nodiscard]] CyEngine engine() noexcept { return &host; }
};

[[nodiscard]] CyLockstepCombatDesc archer() noexcept {
    CyLockstepCombatDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.team = 1;
    desc.health = 60;
    desc.weapon = CY_LOCKSTEP_WEAPON_RANGED;
    desc.damage = 9;
    desc.cooldown = 40;
    desc.range = 8LL << 32;
    desc.projectile_speed = 20LL << 32;
    return desc;
}

[[nodiscard]] CyLockstepStructureDesc tower() noexcept {
    CyLockstepStructureDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.team = 2;
    desc.position = CyFixedVec2{3LL << 32, 4LL << 32};
    desc.radius = 2LL << 32;
    desc.health = 500;
    return desc;
}

}  // namespace

CY_TEST_CASE("lockstep combat: arming and fortifying are configuration, reading is anywhere") {
    Fixture<FakeCombat> fixture;
    const CyInterface& iface = table();
    const CyLockstepCombatDesc arm = archer();
    const CyLockstepStructureDesc building = tower();
    u32 index = 99;
    CY_CHECK_EQ(iface.lockstep_arm(fixture.engine(), 0, &arm), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.lockstep.last_arm.projectile_speed, 20LL << 32);
    CY_CHECK_EQ(iface.lockstep_fortify(fixture.engine(), &building, &index), CY_RESULT_OK);
    CY_CHECK_EQ(index, 0U);
    CY_CHECK_EQ(fixture.lockstep.last_fortify.position.y, 4LL << 32);
    for (const CyPhase phase : {CY_PHASE_NONE, CY_PHASE_FIXED_UPDATE, CY_PHASE_FRAME_UPDATE}) {
        const cy::abi::game::PhaseScope scope(fixture.host.game.clock, phase);
        if (phase != CY_PHASE_NONE) {
            CY_CHECK_EQ(iface.lockstep_arm(fixture.engine(), 0, &arm), CY_RESULT_PERMISSION_DENIED);
            CY_CHECK_EQ(iface.lockstep_fortify(fixture.engine(), &building, &index),
                        CY_RESULT_PERMISSION_DENIED);
        }
        CyLockstepCombatant fighter{};
        fighter.struct_size = sizeof(fighter);
        CY_CHECK_EQ(iface.lockstep_combatant(fixture.engine(), 0, &fighter), CY_RESULT_OK);
        CY_CHECK_EQ(fighter.health, 70U);
        CY_CHECK_EQ(fighter.target_kind, static_cast<u32>(CY_LOCKSTEP_TARGET_STRUCTURE));
        CyLockstepStructure standing{};
        standing.struct_size = sizeof(standing);
        CY_CHECK_EQ(iface.lockstep_structure(fixture.engine(), 0, &standing), CY_RESULT_OK);
        CY_CHECK_EQ(standing.health, 450U);
        u32 count = 0;
        CY_CHECK_EQ(iface.lockstep_events(fixture.engine(), 0, nullptr, 0, &count), CY_RESULT_OK);
        CY_CHECK_EQ(count, 3U);
        CY_CHECK_EQ(iface.lockstep_projectiles(fixture.engine(), nullptr, 0, &count), CY_RESULT_OK);
        CY_CHECK_EQ(count, 1U);
    }
    CY_CHECK_EQ(fixture.lockstep.armed, 1U);
    CY_CHECK_EQ(fixture.lockstep.fortified, 1U);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("lockstep combat: the events from a cursor, into a caller's buffer") {
    Fixture<FakeCombat> fixture;
    const CyInterface& iface = table();
    CyLockstepEvent events[2] = {};
    u32 count = 0;
    CY_CHECK_EQ(iface.lockstep_events(fixture.engine(), 11, events, 2, &count), CY_RESULT_OK);
    CY_CHECK_EQ(count, 2U);
    CY_CHECK_EQ(events[0].sequence, 11U);
    CY_CHECK_EQ(events[1].sequence, 12U);
    CY_CHECK_EQ(iface.lockstep_events(fixture.engine(), 13, events, 2, &count), CY_RESULT_OK);
    CY_CHECK_EQ(count, 0U);
    CyLockstepProjectile flying[1] = {};
    CY_CHECK_EQ(iface.lockstep_projectiles(fixture.engine(), flying, 1, &count), CY_RESULT_OK);
    CY_CHECK_EQ(count, 1U);
    CY_CHECK_EQ(flying[0].id, 7U);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("lockstep combat: the arguments a thunk refuses before a backend sees them") {
    Fixture<FakeCombat> fixture;
    const CyInterface& iface = table();
    CyLockstepCombatDesc arm = archer();
    arm.weapon = CY_LOCKSTEP_WEAPON_RANGED + 1U;
    CY_CHECK_EQ(iface.lockstep_arm(fixture.engine(), 0, &arm), CY_RESULT_INVALID_ARGUMENT);
    arm = archer();
    arm.reserved = 1;
    CY_CHECK_EQ(iface.lockstep_arm(fixture.engine(), 0, &arm), CY_RESULT_INVALID_ARGUMENT);
    arm = archer();
    arm.struct_size = 3;
    CY_CHECK_EQ(iface.lockstep_arm(fixture.engine(), 0, &arm), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.lockstep_arm(fixture.engine(), 0, nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.lockstep_arm(nullptr, 0, &arm), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(fixture.lockstep.armed, 0U);
    // The backend's own refusal passes through.
    arm = archer();
    CY_CHECK_EQ(iface.lockstep_arm(fixture.engine(), 4, &arm), CY_RESULT_NOT_FOUND);

    CyLockstepStructureDesc building = tower();
    u32 index = 0;
    CY_CHECK_EQ(iface.lockstep_fortify(fixture.engine(), &building, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    building.struct_size = 1;
    CY_CHECK_EQ(iface.lockstep_fortify(fixture.engine(), &building, &index),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(fixture.lockstep.fortified, 0U);

    u32 count = 0;
    CY_CHECK_EQ(iface.lockstep_events(fixture.engine(), 0, nullptr, 4, &count),
                CY_RESULT_INVALID_ARGUMENT);
    CyLockstepEvent event{};
    CY_CHECK_EQ(iface.lockstep_events(fixture.engine(), 0, &event, 1, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.lockstep_projectiles(fixture.engine(), nullptr, 2, &count),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.lockstep_combatant(fixture.engine(), 0, nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.lockstep_structure(fixture.engine(), 0, nullptr), CY_RESULT_INVALID_ARGUMENT);
    CyLockstepStructure standing{};
    standing.struct_size = sizeof(standing);
    CY_CHECK_EQ(iface.lockstep_structure(fixture.engine(), 3, &standing), CY_RESULT_NOT_FOUND);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("lockstep combat: struct_size both ways on the combatant") {
    Fixture<FakeCombat> fixture;
    const CyInterface& iface = table();
    CyLockstepCombatant shorter{};
    shorter.struct_size = static_cast<u32>(offsetof(CyLockstepCombatant, max_health));
    shorter.max_health = 0xABCDU;
    CY_CHECK_EQ(iface.lockstep_combatant(fixture.engine(), 0, &shorter), CY_RESULT_OK);
    CY_CHECK_EQ(shorter.health, 70U);
    CY_CHECK_EQ(shorter.max_health, 0xABCDU);
    CY_CHECK_EQ(shorter.struct_size, static_cast<u32>(offsetof(CyLockstepCombatant, max_health)));
    cy::abi::clear_last_error();
}

CY_TEST_CASE("lockstep combat: a session with no combat says so, and no session is UNAVAILABLE") {
    Fixture<PeacefulLockstep> fixture;
    const CyInterface& iface = table();
    const CyLockstepCombatDesc arm = archer();
    CY_CHECK_EQ(iface.lockstep_arm(fixture.engine(), 0, &arm), CY_RESULT_UNSUPPORTED);
    u32 count = 0;
    CY_CHECK_EQ(iface.lockstep_events(fixture.engine(), 0, nullptr, 0, &count),
                CY_RESULT_UNSUPPORTED);
    CyLockstepCombatant fighter{};
    fighter.struct_size = sizeof(fighter);
    CY_CHECK_EQ(iface.lockstep_combatant(fixture.engine(), 0, &fighter), CY_RESULT_UNSUPPORTED);

    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    CY_CHECK_EQ(iface.lockstep_arm(&host, 0, &arm), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(iface.lockstep_projectiles(&host, nullptr, 0, &count), CY_RESULT_UNAVAILABLE);
    cy::abi::clear_last_error();
}
