// SPDX-License-Identifier: MIT
// cy/abi/game/character.h — the backend behind ABI 1.5's `character_*` entries.
// `add-swift-m12-gaps`.
//
// Over `cy::physics::CharacterController` (cy/servers/physics/character.h), one per entity. The
// thunk has checked the phase, the pointers and the vectors, and normalised every sized struct; the
// backend owns the controllers, maps entities to them, applies the description's defaults, and
// answers UNAVAILABLE while the physics step runs, in every build.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/types.h>

namespace cy::abi::game {

/// What `character_*` reaches on the engine side: one capsule controller per entity. A host
/// installs an implementation (`cy::game_backend::CharacterAdapter`); without one the entries
/// return `CY_RESULT_UNAVAILABLE`.
class CharacterBackend {
public:
    virtual ~CharacterBackend() = default;

    /// `character_create`. ALREADY_EXISTS when `entity` has one. `desc` is whole.
    [[nodiscard]] virtual CyResult create(CyEntity entity,
                                          const CyCharacterDesc& desc) noexcept = 0;
    /// `character_destroy`. NOT_FOUND when it has none.
    [[nodiscard]] virtual CyResult destroy(CyEntity entity) noexcept = 0;
    /// `character_move`: one step of `delta` seconds — the clock's fixed delta.
    [[nodiscard]] virtual CyResult move(CyEntity entity, const CyCharacterInput& input,
                                        f32 delta) noexcept = 0;
    /// `character_state`. `out` is whole; the thunk writes the caller's prefix.
    [[nodiscard]] virtual CyResult state(CyEntity entity, CyCharacterState& out) const noexcept = 0;
};

}  // namespace cy::abi::game
