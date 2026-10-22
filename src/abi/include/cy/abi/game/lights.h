// SPDX-License-Identifier: MIT
// cy/abi/game/lights.h — the backend behind ABI 1.11's lights.
//
// `cy_abi` cannot name `cy::rendering-lighting`: the table is in every build and a dedicated server
// draws nothing. So the pool is reached through this backend, which `cy::game_backend::LightsAdapter`
// (src/game_backend/lights/) implements over `cy::rendering::TransientLights`. A host that draws
// nothing binds none, and every 1.11 entry answers UNAVAILABLE.
//
// The thunks (src/abi/src/game/light_thunks.cpp) check the engine, the phase, the binding and the
// pointers, normalise every sized struct and refuse a kind, a fade, a flag or a negative number out
// of range; a backend validates DOMAIN facts — a light that ended, a full pool — and reports them
// with `cy::abi::report`, returning the code.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/types.h>

namespace cy::abi::game {

/// The lights a game makes, as the 1.11 entries see them. Every method is called on the game
/// thread, in no phase or a frame update.
class LightsBackend {
public:
    virtual ~LightsBackend() = default;

    /// `desc` is whole, its kind, fade and flags in range and its numbers not negative.
    [[nodiscard]] virtual CyResult spawn(const CyLightDesc& desc, CyLight& out) noexcept = 0;
    [[nodiscard]] virtual CyResult update(CyLight light, const CyLightDesc& desc) noexcept = 0;
    [[nodiscard]] virtual CyResult destroy(CyLight light) noexcept = 0;
    [[nodiscard]] virtual CyResult alive(CyLight light, u32& out) const noexcept = 0;
    [[nodiscard]] virtual CyResult status(CyLightStatus& out) const noexcept = 0;
};

}  // namespace cy::abi::game
