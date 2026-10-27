// SPDX-License-Identifier: MIT
#pragma once
// The effects adapter behind ABI 1.15's `effect_*` entries: the effects a host cooked, registered
// by name, played into its `cy::vfx::SimulationWorld`.
//
// The HOST owns the world and the compiled systems. It registers each effect a game may play under
// a name (`add_effect`), binds the adapter, and runs the world as it always does: once a frame it
// steps it and publishes its sprites into the frame's particle renderer. A game resolves a name
// once (`effect_find`) and plays the handle fire-and-forget (`effect_play`): the instance releases
// itself when its last particle dies. Nothing here draws, and nothing feeds back into a fixed step.

#include <cy/abi/cy_abi.h>
#include <cy/abi/game/effects.h>
#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/vfx/compile.h>
#include <cy/vfx/world.h>

namespace cy::game_backend {

class EffectsAdapter final : public abi::game::EffectsBackend {
public:
    /// Plays into `world`, which outlives the adapter.
    EffectsAdapter(Allocator& allocator, vfx::SimulationWorld& world) noexcept;

    /// Register `system` under `name`; its handle is its registration order, from one. The system
    /// outlives the adapter: the world holds a pointer to it for every instance it plays.
    /// `spawn_seconds` is how long one play spawns — a burst's fraction of a second — after which
    /// its particles live out their lifetimes and the instance releases itself; zero spawns until
    /// the world is shut down, which only an effect a game never stops should be. Refuses a name
    /// already registered.
    [[nodiscard]] Status add_effect(const char* name, const vfx::CompiledSystem& system,
                                    f32 spawn_seconds = 0.0F) noexcept;

    /// Effects played, and plays the world refused, since the adapter was made.
    [[nodiscard]] u64 played() const noexcept { return played_; }
    [[nodiscard]] u64 refused() const noexcept { return refused_; }
    /// Plays of effect `handle`, for a host's report.
    [[nodiscard]] u64 played(CyEffect handle) const noexcept;

    // --- abi::game::EffectsBackend ---------------------------------------------------------------
    [[nodiscard]] CyResult find(const char* name, CyEffect& out) const noexcept override;
    [[nodiscard]] CyResult play(CyEffect effect, const CyEffectPlay& play) noexcept override;

private:
    struct Entry {
        Name name;
        const vfx::CompiledSystem* system = nullptr;
        f32 spawn_seconds = 0.0F;
        u64 plays = 0;
    };

    vfx::SimulationWorld* world_;
    Array<Entry> effects_;
    u64 played_ = 0;
    u64 refused_ = 0;
};

/// Bind `adapter` as `host.game.effects`, or unbind with null.
void bind_effects(cy::abi::Host& host, EffectsAdapter* adapter) noexcept;

}  // namespace cy::game_backend
