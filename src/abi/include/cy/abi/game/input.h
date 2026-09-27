// SPDX-License-Identifier: MIT
// cy/abi/game/input.h — the backend behind ABI 1.3's `input_*` entries. `add-swift-game-api`.
//
// OWNER: implementer A (input and camera). See openspec/changes/add-swift-game-api/design.md.
//
// Every method is called by a thunk that has already checked the engine, the phase, the backend and
// every required pointer (cy/abi/game/services.h), so a method validates only domain facts — an
// unknown action or context name, a user the server does not have — and reports them with
// `cy::abi::report`, returning the code.
//
// DETERMINISM. `action_state` must answer from the state the input server resolved for a tick —
// `cy::input::InputUser::action_state` after `InputServer::resolve_tick` — never from live device
// state, because it is callable in a fixed step and a replay must read what the original run read.
// `pointer` and `modifiers` are device state; their thunks refuse them in a fixed step.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/types.h>

namespace cy::abi::game {

/// The input server as ABI 1.3's `input_*` entries see it: resolved action state, the pointer,
/// the modifier keys and the mapping-context stack, per input user.
class InputBackend {
public:
    virtual ~InputBackend() = default;

    /// `input_find_action`: the dense index of the action declared under `name`; NOT_FOUND.
    [[nodiscard]] virtual CyResult find_action(const char* name,
                                               CyInputAction& out_action) noexcept = 0;

    /// `input_action_state`: `out_state` arrives zeroed with `struct_size == sizeof`. NOT_FOUND for
    /// an invalid action, OUT_OF_RANGE for a user the server does not have.
    [[nodiscard]] virtual CyResult action_state(u32 user, CyInputAction action,
                                                CyInputActionState& out_state) noexcept = 0;

    /// `input_pointer`: position and edges since the previous frame update. A user without a
    /// pointing device is CY_RESULT_OK with CY_INPUT_POINTER_PRESENT clear.
    [[nodiscard]] virtual CyResult pointer(u32 user, CyInputPointer& out_pointer) noexcept = 0;

    /// `input_modifiers`: CY_INPUT_MOD_* held now.
    [[nodiscard]] virtual CyResult modifiers(u32 user, u32& out_modifiers) noexcept = 0;

    /// `input_find_context`: the mapping context registered under `name`; NOT_FOUND.
    [[nodiscard]] virtual CyResult find_context(const char* name,
                                                CyInputContext& out_context) noexcept = 0;

    /// `input_push_context`: effective from the next tick's resolution. ALREADY_EXISTS when it is
    /// already on that user's stack; NOT_FOUND for an unknown context.
    [[nodiscard]] virtual CyResult push_context(u32 user, CyInputContext context,
                                                i32 priority) noexcept = 0;

    /// `input_pop_context`: NOT_FOUND when the context is not on that user's stack.
    [[nodiscard]] virtual CyResult pop_context(u32 user, CyInputContext context) noexcept = 0;
};

}  // namespace cy::abi::game
