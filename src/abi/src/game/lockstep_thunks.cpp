// SPDX-License-Identifier: MIT
// The lockstep entries of ABI 1.8 (openspec/changes/add-deterministic-math stage 8) and the combat
// entries of ABI 1.16 (openspec/changes/add-rts-combat-and-minimap).
//
// Enlisting is session configuration (`[N]`): units join before the first tick. An order is
// recorded for the next tick from a fixed step or a frame (`[F U]`), because a click is a frame's
// and a scripted order a step's, and either way the command stream, not the caller's phase, decides
// the tick it executes on. Reading answers anywhere.

#include <cy/abi/errors.h>
#include <cy/abi/game/lockstep.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>

#include "thunks.h"

namespace cy::abi::game {
namespace {

constexpr u32 kOrderPhases = kPhaseFixed | kPhaseFrame;

[[nodiscard]] LockstepBackend* enter(CyEngine engine, u32 phases, const char* entry) noexcept {
    if (engine == nullptr) {
        (void)report(CY_RESULT_INVALID_ARGUMENT, "the engine is null");
        return nullptr;
    }
    if (require_phase(engine->game, phases, entry) != CY_RESULT_OK) {
        return nullptr;
    }
    if (engine->game.lockstep == nullptr) {
        (void)report(CY_RESULT_UNAVAILABLE, "no lockstep session is bound to this engine");
    }
    return engine->game.lockstep;
}

[[nodiscard]] CyResult finish(CyResult result) noexcept {
    if (result == CY_RESULT_OK) {
        clear_last_error();
    }
    return result;
}

/// Ask the backend for a whole struct and write the caller's prefix of it.
template <class T, class Fn>
[[nodiscard]] CyResult answer(T* out, const char* malformed, Fn&& ask) noexcept {
    if (agreed_size<T>(out->struct_size) == 0U) {
        return report(CY_RESULT_INVALID_ARGUMENT, malformed);
    }
    T whole{};
    whole.struct_size = static_cast<u32>(sizeof(T));
    if (const CyResult result = ask(whole); result != CY_RESULT_OK) {
        return result;
    }
    (void)write_sized(*out, whole);
    clear_last_error();
    return CY_RESULT_OK;
}

/// A combat entry on a backend that has no combat.
[[nodiscard]] CyResult no_combat() noexcept {
    return report(CY_RESULT_UNSUPPORTED, "this lockstep session has no combat");
}

/// The sizing pattern of `world_chunks`: a null buffer only with zero capacity.
[[nodiscard]] bool listed(const void* out, u32 capacity, const uint32_t* out_count) noexcept {
    return out_count != nullptr && (out != nullptr || capacity == 0U);
}

}  // namespace

// --- 1.16: the default backend answers, for a session with no combat
// ------------------------------

CyResult LockstepBackend::arm(u32 /*unit*/, const CyLockstepCombatDesc& /*desc*/) noexcept {
    return no_combat();
}

CyResult LockstepBackend::fortify(const CyLockstepStructureDesc& /*desc*/,
                                  u32& /*out_structure*/) noexcept {
    return no_combat();
}

CyResult LockstepBackend::combatant(u32 /*unit*/, CyLockstepCombatant& /*out*/) const noexcept {
    return no_combat();
}

CyResult LockstepBackend::structure(u32 /*index*/, CyLockstepStructure& /*out*/) const noexcept {
    return no_combat();
}

CyResult LockstepBackend::events(u64 /*first*/, CyLockstepEvent* /*out*/, u32 /*capacity*/,
                                 u32& /*out_count*/) const noexcept {
    return no_combat();
}

CyResult LockstepBackend::projectiles(CyLockstepProjectile* /*out*/, u32 /*capacity*/,
                                      u32& /*out_count*/) const noexcept {
    return no_combat();
}

CyResult lockstep_enlist(CyEngine engine, const CyLockstepUnitDesc* desc, uint32_t* out_unit) {
    LockstepBackend* backend = enter(engine, kPhaseNone, "lockstep_enlist");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (desc == nullptr || out_unit == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "lockstep_enlist needs a desc and an output");
    }
    CyLockstepUnitDesc whole{};
    if (!read_sized(*desc, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "the unit desc has a malformed struct_size");
    }
    if (whole.radius < 0 || whole.max_speed < 0) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "a unit's radius and speed are not negative; zero is the default");
    }
    u32 unit = 0;
    const CyResult result = backend->enlist(whole, unit);
    if (result == CY_RESULT_OK) {
        *out_unit = unit;
    }
    return finish(result);
}

CyResult lockstep_order(CyEngine engine, const CyLockstepOrder* order) {
    LockstepBackend* backend = enter(engine, kOrderPhases, "lockstep_order");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (order == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "lockstep_order needs an order");
    }
    CyLockstepOrder whole{};
    if (!read_sized(*order, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "the order has a malformed struct_size");
    }
    if (whole.kind > static_cast<u32>(CY_LOCKSTEP_ORDER_ATTACK)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "an order's kind is a CyLockstepOrderKind");
    }
    return finish(backend->order(whole));
}

CyResult lockstep_unit(CyEngine engine, uint32_t unit, CyLockstepUnit* out_unit) {
    LockstepBackend* backend = enter(engine, kPhaseAny, "lockstep_unit");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (out_unit == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "lockstep_unit needs an output");
    }
    return answer(out_unit, "the unit has a malformed struct_size",
                  [&](CyLockstepUnit& whole) noexcept { return backend->unit(unit, whole); });
}

CyResult lockstep_status(CyEngine engine, CyLockstepStatus* out_status) {
    LockstepBackend* backend = enter(engine, kPhaseAny, "lockstep_status");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (out_status == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "lockstep_status needs an output");
    }
    return answer(out_status, "the status has a malformed struct_size",
                  [&](CyLockstepStatus& whole) noexcept { return backend->status(whole); });
}

// --- 1.16: combat -------------------------------------------------------------------------------

CyResult lockstep_arm(CyEngine engine, uint32_t unit, const CyLockstepCombatDesc* desc) {
    LockstepBackend* backend = enter(engine, kPhaseNone, "lockstep_arm");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (desc == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "lockstep_arm needs a desc");
    }
    CyLockstepCombatDesc whole{};
    if (!read_sized(*desc, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "the combat desc has a malformed struct_size");
    }
    if (whole.weapon > static_cast<u32>(CY_LOCKSTEP_WEAPON_RANGED) || whole.reserved != 0U) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "a weapon is a CyLockstepWeapon, and the reserved word is zero");
    }
    return finish(backend->arm(unit, whole));
}

CyResult lockstep_fortify(CyEngine engine, const CyLockstepStructureDesc* desc,
                          uint32_t* out_structure) {
    LockstepBackend* backend = enter(engine, kPhaseNone, "lockstep_fortify");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (desc == nullptr || out_structure == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "lockstep_fortify needs a desc and an output");
    }
    CyLockstepStructureDesc whole{};
    if (!read_sized(*desc, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "the structure desc has a malformed struct_size");
    }
    u32 structure = 0;
    const CyResult result = backend->fortify(whole, structure);
    if (result == CY_RESULT_OK) {
        *out_structure = structure;
    }
    return finish(result);
}

CyResult lockstep_combatant(CyEngine engine, uint32_t unit, CyLockstepCombatant* out_combatant) {
    LockstepBackend* backend = enter(engine, kPhaseAny, "lockstep_combatant");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (out_combatant == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "lockstep_combatant needs an output");
    }
    return answer(
        out_combatant, "the combatant has a malformed struct_size",
        [&](CyLockstepCombatant& whole) noexcept { return backend->combatant(unit, whole); });
}

CyResult lockstep_structure(CyEngine engine, uint32_t structure,
                            CyLockstepStructure* out_structure) {
    LockstepBackend* backend = enter(engine, kPhaseAny, "lockstep_structure");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (out_structure == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "lockstep_structure needs an output");
    }
    return answer(
        out_structure, "the structure has a malformed struct_size",
        [&](CyLockstepStructure& whole) noexcept { return backend->structure(structure, whole); });
}

CyResult lockstep_events(CyEngine engine, uint64_t first, CyLockstepEvent* out_events,
                         uint32_t capacity, uint32_t* out_count) {
    LockstepBackend* backend = enter(engine, kPhaseAny, "lockstep_events");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (!listed(out_events, capacity, out_count)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "lockstep_events needs a count, and a buffer unless its capacity is zero");
    }
    u32 count = 0;
    const CyResult result = backend->events(first, out_events, capacity, count);
    if (result == CY_RESULT_OK) {
        *out_count = count;
    }
    return finish(result);
}

CyResult lockstep_projectiles(CyEngine engine, CyLockstepProjectile* out_projectiles,
                              uint32_t capacity, uint32_t* out_count) {
    LockstepBackend* backend = enter(engine, kPhaseAny, "lockstep_projectiles");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (!listed(out_projectiles, capacity, out_count)) {
        return report(
            CY_RESULT_INVALID_ARGUMENT,
            "lockstep_projectiles needs a count, and a buffer unless its capacity is zero");
    }
    u32 count = 0;
    const CyResult result = backend->projectiles(out_projectiles, capacity, count);
    if (result == CY_RESULT_OK) {
        *out_count = count;
    }
    return finish(result);
}

}  // namespace cy::abi::game
