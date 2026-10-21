// SPDX-License-Identifier: MIT
// cy/abi/game/fog.h — the backend behind ABI 1.10's fog of war entries.
//
// `cy_abi` cannot name `cy::gameplay-fog`: the table is in every build and a game without fog of
// war links none of it. So the grid is reached through this backend, which
// `cy::game_backend::FogAdapter` (src/game_backend/fog/) implements over
// `cy::gameplay::fog::FogOfWar`. A host that runs no fog binds none, and every 1.10 entry answers
// UNAVAILABLE.
//
// The thunks (src/abi/src/game/fog_thunks.cpp) check the engine, the phase, the binding and the
// pointers, normalise every sized struct and refuse a team, a flag or a radius out of range; a
// backend validates DOMAIN facts — an entity the host does not place — and reports them with
// `cy::abi::report`, returning the code.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/types.h>

namespace cy::abi::game {

/// The fog of war, as the 1.10 entries see it. Every method is called on the game thread.
class FogBackend {
public:
    virtual ~FogBackend() = default;

    /// `vision` is whole: its team at most 32, its radius not negative, its flags known.
    [[nodiscard]] virtual CyResult set_vision(CyEntity entity,
                                              const CyFogVision& vision) noexcept = 0;
    [[nodiscard]] virtual CyResult vision(CyEntity entity, CyFogVision& out) const noexcept = 0;
    /// A `CyFogCell`.
    [[nodiscard]] virtual CyResult cell(u32 team, const CyFixedVec2& position,
                                        u32& out) const noexcept = 0;
    /// A `CyFogSight`.
    [[nodiscard]] virtual CyResult sight(u32 team, CyEntity entity, u32& out) const noexcept = 0;
    [[nodiscard]] virtual CyResult status(CyFogStatus& out) const noexcept = 0;
};

}  // namespace cy::abi::game
