// SPDX-License-Identifier: MIT
// The lockstep entries of ABI 1.8. openspec/changes/add-deterministic-math stage 8.
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

}  // namespace

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
    if (whole.kind > static_cast<u32>(CY_LOCKSTEP_ORDER_STOP)) {
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

}  // namespace cy::abi::game
