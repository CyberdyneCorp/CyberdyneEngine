// SPDX-License-Identifier: MIT
// cy/abi/game/decals.h — the backend behind ABI 1.15's decals.
//
// `cy_abi` cannot name `cy::rendering-lighting`: the table is in every build and a dedicated server
// draws nothing. So the decal budget is reached through this backend, which
// `cy::game_backend::DecalsAdapter` (src/game_backend/decals/) implements over
// `cy::rendering::DecalBudget`. A host that draws nothing binds none, and every `decal_*` entry
// answers UNAVAILABLE.
//
// The thunks (src/abi/src/game/decal_thunks.cpp) check the engine, the phase, the binding and the
// pointers, normalise every sized struct and refuse a number out of range; a backend validates
// DOMAIN facts — a name it has not registered, a material it did not hand out — and reports them
// with `cy::abi::report`, returning the code.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/types.h>

namespace cy::abi::game {

/// The decals a game spawns, as the 1.15 entries see them. Every method is called on the game
/// thread; `find_material` in any phase, the rest in no phase or a frame update.
class DecalsBackend {
public:
    virtual ~DecalsBackend() = default;

    /// `name` is not null.
    [[nodiscard]] virtual CyResult find_material(const char* name,
                                                 CyDecalMaterial& out) const noexcept = 0;
    /// `desc` is whole, its extents positive, its importance and fades in range, its numbers
    /// finite. `out` is the budget's id, or CY_DECAL_NULL when the new decal was the one evicted.
    [[nodiscard]] virtual CyResult spawn(const CyDecalDesc& desc, CyDecal& out) noexcept = 0;
    [[nodiscard]] virtual CyResult status(CyDecalStatus& out) const noexcept = 0;
};

}  // namespace cy::abi::game
