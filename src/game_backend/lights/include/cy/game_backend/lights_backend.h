// SPDX-License-Identifier: MIT
#pragma once
// The lights adapter behind ABI 1.11's `light_*` entries: `cy::rendering::TransientLights` with the
// conversion from the ABI's description to the engine's light.
//
// A game spawns a light through `light_spawn` — a muzzle flash for fifty milliseconds, a torch
// until it is destroyed — and the HOST runs the pool: once a frame it calls `advance(seconds)`,
// which ages every light and frees those whose life ended, and `append_to(lights)`, which appends
// the live ones to the frame's own, each under its fade. From there a flash is a light like any
// other: clustered, sampled by the stochastic path, handed to the GI. Nothing here draws.

#include <cy/abi/cy_abi.h>
#include <cy/abi/game/lights.h>
#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/lighting/transient_lights.h>
#include <cy/servers/render/model.h>

namespace cy::game_backend {

/// The engine's light for an ABI description: the kind, the pose, the colour, the intensity in the
/// kind's own unit (candela, or nits for an area light), the cone, the size, two-sidedness and the
/// channels — every light a game spawns is `Movable`.
[[nodiscard]] render::LightDescription light_of(const CyLightDesc& desc) noexcept;

/// The ABI's description of an engine light: `light_of`'s inverse over every field it carries.
[[nodiscard]] CyLightDesc desc_of(const render::LightDescription& light, f32 lifetime,
                                  rendering::TransientFade fade) noexcept;

class LightsAdapter final : public abi::game::LightsBackend {
public:
    explicit LightsAdapter(Allocator& allocator) noexcept;

    /// The pool's size: the most lights a game may have alive at once.
    [[nodiscard]] Status create(u32 capacity) noexcept;
    /// One frame: every light aged by `seconds`, those whose life ended freed.
    void advance(f32 seconds) noexcept;
    /// Append every live light, under its fade, to the frame's lights. Returns how many.
    [[nodiscard]] Expected<u32, Error> append_to(
        Array<render::LightDescription>& out) const noexcept;

    [[nodiscard]] const rendering::TransientLights& pool() const noexcept { return pool_; }

    // --- abi::game::LightsBackend ----------------------------------------------------------------
    [[nodiscard]] CyResult spawn(const CyLightDesc& desc, CyLight& out) noexcept override;
    [[nodiscard]] CyResult update(CyLight light, const CyLightDesc& desc) noexcept override;
    [[nodiscard]] CyResult destroy(CyLight light) noexcept override;
    [[nodiscard]] CyResult alive(CyLight light, u32& out) const noexcept override;
    [[nodiscard]] CyResult status(CyLightStatus& out) const noexcept override;

private:
    rendering::TransientLights pool_;
};

/// Bind `adapter` as `host.game.lights`, or unbind with null.
void bind_lights(cy::abi::Host& host, LightsAdapter* adapter) noexcept;

}  // namespace cy::game_backend
