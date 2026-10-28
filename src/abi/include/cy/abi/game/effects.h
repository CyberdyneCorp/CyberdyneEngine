// SPDX-License-Identifier: MIT
// cy/abi/game/effects.h — the backend behind ABI 1.15's effects.
//
// `cy_abi` cannot name `cy::vfx`: the table is in every build, and `-D CY_VFX=OFF` removes the VFX
// system. So the simulation world is reached through this backend, which
// `cy::game_backend::EffectsAdapter` (src/game_backend/effects/) implements over
// `cy::vfx::SimulationWorld`. A host that draws nothing binds none, and every `effect_*` entry
// answers UNAVAILABLE.
//
// The thunks (src/abi/src/game/effect_thunks.cpp) check the engine, the phase, the binding and the
// pointers, normalise the sized struct and refuse a count, a scale or a number out of range; a
// backend validates DOMAIN facts — a name it has not registered, an effect it did not hand out, a
// full world — and reports them with `cy::abi::report`, returning the code.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/types.h>

namespace cy::abi::game {

/// The effects a game plays, as the 1.15 entries see them. Every method is called on the game
/// thread; `find` in any phase, `play` in no phase or a frame update.
class EffectsBackend {
public:
    virtual ~EffectsBackend() = default;

    /// `name` is not null.
    [[nodiscard]] virtual CyResult find(const char* name, CyEffect& out) const noexcept = 0;
    /// `play` is whole, its count at most eight, its scale positive and its numbers finite.
    [[nodiscard]] virtual CyResult play(CyEffect effect, const CyEffectPlay& play) noexcept = 0;
};

}  // namespace cy::abi::game
