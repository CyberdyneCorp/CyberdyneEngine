// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_API_BATTLE_EFFECTS_H
#define CY_SAMPLE_RTS_API_BATTLE_EFFECTS_H
// battle_effects.h — what the battle looks like: five VFX systems the Swift game plays through ABI
// 1.15's `effect_play`, the simulation world they play into, and the transient lights its
// explosions flash with (ABI 1.11). Built only with `--battle` and CY_VFX.
//
//   battle.launch      a puff of dust and grit thrown along the shot as a bow or a catapult looses
//   battle.trail       one projectile's mark this frame: a hot head and two wisps of smoke that
//                      linger, so a projectile played every frame draws its own trail behind it
//   battle.sparks      sparks thrown off a hit along the struck surface's normal, falling, and
//                      BOUNCING off the ground — the ground's height is the `floor` parameter the
//                      game passes, and the update reflects a spark that went through it, damped
//   battle.explosion   a catapult's shot or a falling building: a fireball and embers flung out,
//                      the embers bouncing off the same `floor`
//   battle.smoke       slow grey smoke that rises and spreads: after a blast, and over a building
//                      the battle has damaged
//
// Each system is authored as the FPS sample's shot effects are (`src/vfx/tests/effects.h`'s
// `StageBuilder`, compiled by the shipping compiler) and registered with a spawn duration, so a
// play the game forgets releases itself once its last particle has died.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/graph/cybergraph.h>
#include <cy/rendering/particles/particle_renderer.h>
#include <cy/vfx/compile.h>
#include <cy/vfx/runtime.h>
#include <cy/vfx/world.h>

#include <memory>

namespace sample::rts {

/// The names the game plays the effects by. game/Battle.swift's `BattleLook` names them too.
inline constexpr const char* kLaunchEffect = "battle.launch";
inline constexpr const char* kTrailEffect = "battle.trail";
inline constexpr const char* kSparksEffect = "battle.sparks";
inline constexpr const char* kExplosionEffect = "battle.explosion";
inline constexpr const char* kSmokeEffect = "battle.smoke";
inline constexpr cy::u32 kBattleEffects = 5;

/// The particle ring a frame publishes into.
inline constexpr cy::u32 kBattleParticleRing = 8192;

class BattleEffects {
public:
    explicit BattleEffects(cy::Allocator& allocator) noexcept;
    ~BattleEffects();

    BattleEffects(const BattleEffects&) = delete;
    BattleEffects& operator=(const BattleEffects&) = delete;

    /// Cook the five systems and make the world.
    [[nodiscard]] cy::Status build() noexcept;

    /// One frame of the world.
    [[nodiscard]] cy::Status step(cy::f32 dt) noexcept;
    /// The live sprites, camera-relative to `eye`, into `out`.
    [[nodiscard]] cy::Status publish(
        cy::Vec3 eye, cy::Array<cy::rendering::particles::ParticleInstance>& out) const noexcept;

    [[nodiscard]] cy::vfx::SimulationWorld& world() noexcept { return world_; }
    /// System `index` in the order of the names above, and how long one play of it spawns.
    [[nodiscard]] const cy::vfx::CompiledSystem& system(cy::u32 index) const noexcept {
        return *systems_[index];
    }
    [[nodiscard]] static const char* name(cy::u32 index) noexcept;
    [[nodiscard]] static cy::f32 spawn_seconds(cy::u32 index) noexcept;

    [[nodiscard]] cy::u32 peak_particles() const noexcept { return peak_particles_; }
    [[nodiscard]] cy::u64 particle_frames() const noexcept { return particle_frames_; }

private:
    cy::Allocator* allocator_;
    cy::graph::DiagnosticSink sink_;
    std::unique_ptr<cy::vfx::CompiledSystem> systems_[kBattleEffects];
    cy::vfx::SimulationWorld world_;
    cy::vfx::StepReport stepped_;
    cy::u32 peak_particles_ = 0;
    cy::u64 particle_frames_ = 0;
};

}  // namespace sample::rts

#endif  // CY_SAMPLE_RTS_API_BATTLE_EFFECTS_H
