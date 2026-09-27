// SPDX-License-Identifier: MIT
// The `input` thunks of ABI 1.3. `add-swift-game-api`.
//
// OWNER: implementer A (input and camera). See openspec/changes/add-swift-game-api/design.md.
//
// Each thunk takes cy/abi/game/services.h's steps in order — engine, phase, backend, pointers,
// `struct_size` — and only then asks the `InputBackend`. What reaches the backend is always whole:
// a non-null name, a zeroed result struct of this build's size. The caller's out-parameter is
// written only after the backend answered OK, so a failure leaves it untouched.
//
// PHASES. Action state and the context stack are simulation state and are callable everywhere
// (`[N F U]`); the pointer and the modifier keys are device state and refuse a fixed step
// (`[N U]`), which is what keeps a fixed step replayable.

#include <cy/abi/errors.h>
#include <cy/abi/game/input.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>

#include "thunks.h"

namespace cy::abi::game {
namespace {

/// Device state: a frame update or no phase, never a fixed step.
constexpr u32 kDevicePhases = kPhaseNone | kPhaseFrame;

/// Steps 1 to 3: the engine, the phase, the backend. Null, with `result` set, when any refuses —
/// returned as the pointer rather than through an out-parameter so that every use after the check
/// is visibly non-null.
[[nodiscard]] InputBackend* enter(CyEngine engine, u32 phases, const char* entry,
                                  CyResult& result) noexcept {
    result = CY_RESULT_OK;
    if (engine == nullptr) {
        result = report(CY_RESULT_INVALID_ARGUMENT, "the engine is null");
        return nullptr;
    }
    if (const CyResult allowed = require_phase(engine->game, phases, entry);
        allowed != CY_RESULT_OK) {
        result = allowed;
        return nullptr;
    }
    if (engine->game.input == nullptr) {
        result = report(CY_RESULT_UNAVAILABLE, "no input backend is bound to this engine");
    }
    return engine->game.input;
}

[[nodiscard]] CyResult null_argument(const char* what) noexcept {
    return report(CY_RESULT_INVALID_ARGUMENT, what);
}

/// Whether a caller's out-struct has a `struct_size` this build can write. Checked before the
/// backend runs, so a malformed struct is refused without side effects.
template <class T>
[[nodiscard]] bool writable(const T& out) noexcept {
    return agreed_size<T>(out.struct_size) != 0U;
}

/// `action_state` for a resolved action, written into the caller's struct.
[[nodiscard]] CyResult read_action_state(InputBackend& backend, u32 user, CyInputAction action,
                                         CyInputActionState& out_state) noexcept {
    CyInputActionState state{};
    state.struct_size = sizeof(CyInputActionState);
    if (const CyResult result = backend.action_state(user, action, state); result != CY_RESULT_OK) {
        return result;
    }
    (void)write_sized(out_state, state);
    clear_last_error();
    return CY_RESULT_OK;
}

}  // namespace

CyResult input_find_action(CyEngine engine, const char* name, CyInputAction* out_action) {
    CyResult entered = CY_RESULT_OK;
    InputBackend* backend = enter(engine, kPhaseAny, "input_find_action", entered);
    if (backend == nullptr) {
        return entered;
    }
    if (name == nullptr || out_action == nullptr) {
        return null_argument("input_find_action needs a name and an output");
    }
    CyInputAction action = CY_INPUT_ACTION_INVALID;
    if (const CyResult result = backend->find_action(name, action); result != CY_RESULT_OK) {
        return result;
    }
    *out_action = action;
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult input_action_state(CyEngine engine, uint32_t user, CyInputAction action,
                            CyInputActionState* out_state) {
    CyResult entered = CY_RESULT_OK;
    InputBackend* backend = enter(engine, kPhaseAny, "input_action_state", entered);
    if (backend == nullptr) {
        return entered;
    }
    if (out_state == nullptr) {
        return null_argument("input_action_state needs an output");
    }
    if (!writable(*out_state)) {
        return null_argument("input_action_state: malformed struct_size");
    }
    return read_action_state(*backend, user, action, *out_state);
}

CyResult input_action_state_by_name(CyEngine engine, uint32_t user, const char* name,
                                    CyInputActionState* out_state) {
    CyResult entered = CY_RESULT_OK;
    InputBackend* backend = enter(engine, kPhaseAny, "input_action_state_by_name", entered);
    if (backend == nullptr) {
        return entered;
    }
    if (name == nullptr || out_state == nullptr) {
        return null_argument("input_action_state_by_name needs a name and an output");
    }
    if (!writable(*out_state)) {
        return null_argument("input_action_state_by_name: malformed struct_size");
    }
    CyInputAction action = CY_INPUT_ACTION_INVALID;
    if (const CyResult found = backend->find_action(name, action); found != CY_RESULT_OK) {
        return found;
    }
    return read_action_state(*backend, user, action, *out_state);
}

CyResult input_pointer(CyEngine engine, uint32_t user, CyInputPointer* out_pointer) {
    CyResult entered = CY_RESULT_OK;
    InputBackend* backend = enter(engine, kDevicePhases, "input_pointer", entered);
    if (backend == nullptr) {
        return entered;
    }
    if (out_pointer == nullptr) {
        return null_argument("input_pointer needs an output");
    }
    if (!writable(*out_pointer)) {
        return null_argument("input_pointer: malformed struct_size");
    }
    CyInputPointer pointer{};
    pointer.struct_size = sizeof(CyInputPointer);
    if (const CyResult result = backend->pointer(user, pointer); result != CY_RESULT_OK) {
        return result;
    }
    (void)write_sized(*out_pointer, pointer);
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult input_modifiers(CyEngine engine, uint32_t user, uint32_t* out_modifiers) {
    CyResult entered = CY_RESULT_OK;
    InputBackend* backend = enter(engine, kDevicePhases, "input_modifiers", entered);
    if (backend == nullptr) {
        return entered;
    }
    if (out_modifiers == nullptr) {
        return null_argument("input_modifiers needs an output");
    }
    u32 modifiers = 0;
    if (const CyResult result = backend->modifiers(user, modifiers); result != CY_RESULT_OK) {
        return result;
    }
    *out_modifiers = modifiers;
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult input_find_context(CyEngine engine, const char* name, CyInputContext* out_context) {
    CyResult entered = CY_RESULT_OK;
    InputBackend* backend = enter(engine, kPhaseAny, "input_find_context", entered);
    if (backend == nullptr) {
        return entered;
    }
    if (name == nullptr || out_context == nullptr) {
        return null_argument("input_find_context needs a name and an output");
    }
    CyInputContext context = CY_INPUT_CONTEXT_NULL;
    if (const CyResult result = backend->find_context(name, context); result != CY_RESULT_OK) {
        return result;
    }
    *out_context = context;
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult input_push_context(CyEngine engine, uint32_t user, CyInputContext context,
                            int32_t priority) {
    CyResult entered = CY_RESULT_OK;
    InputBackend* backend = enter(engine, kPhaseAny, "input_push_context", entered);
    if (backend == nullptr) {
        return entered;
    }
    if (const CyResult result = backend->push_context(user, context, priority);
        result != CY_RESULT_OK) {
        return result;
    }
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult input_pop_context(CyEngine engine, uint32_t user, CyInputContext context) {
    CyResult entered = CY_RESULT_OK;
    InputBackend* backend = enter(engine, kPhaseAny, "input_pop_context", entered);
    if (backend == nullptr) {
        return entered;
    }
    if (const CyResult result = backend->pop_context(user, context); result != CY_RESULT_OK) {
        return result;
    }
    clear_last_error();
    return CY_RESULT_OK;
}

}  // namespace cy::abi::game
