// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_FIRST_PERSON_SHOT_EFFECTS_H
#define CY_SAMPLE_FIRST_PERSON_SHOT_EFFECTS_H
// shot_effects.h — what a shot leaves behind: two VFX systems and a decal material, and the
// simulation world and decal budget the game plays them into through ABI 1.15.
//
//   weapon.muzzle   a burst of hot sprites thrown along the barrel for a tenth of a second
//   impact.sparks   a shower of sparks thrown off the struck surface along its normal, falling
//                   under gravity and BOUNCING OFF THE FLOOR: the floor's height is a parameter
//                   the game passes (its `y`, in the effect's own space), and the update stage
//                   reflects a spark that went through it, damped
//   impact.scorch   the mark the shot leaves: a dark, rough splat, a little proud of the surface
//
// The two effects are authored as `src/vfx/tests/effects.cpp` authors the spark plume and
// samples/12-beauty authors its embers — `cy::graph::Graph`s built node by node and compiled by the
// shipping compiler — because there is no on-disk VFX asset a sample can read without the editor.
// Both are registered with a spawn duration (`EffectSpawn::spawn_seconds`), so a play the game
// forgets releases itself once its last particle has died.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/graph/cybergraph.h>
#include <cy/rendering/decals/decal_table.h>
#include <cy/rendering/lighting/decals.h>
#include <cy/rendering/particles/particle_renderer.h>
#include <cy/vfx/compile.h>
#include <cy/vfx/runtime.h>
#include <cy/vfx/world.h>

#include <memory>
#include <vector>

namespace sample::fps {

/// The particle ring the renderer draws from: the most sprites a frame publishes.
inline constexpr cy::u32 kParticleRing = 2048;
/// The decal budget: the most marks the arena keeps before the oldest go.
inline constexpr cy::u32 kDecalCapacity = 96;

class ShotEffects {
public:
    explicit ShotEffects(cy::Allocator& allocator) noexcept;
    ~ShotEffects();

    ShotEffects(const ShotEffects&) = delete;
    ShotEffects& operator=(const ShotEffects&) = delete;

    /// Cook both systems and make the world and the budget.
    [[nodiscard]] cy::Status build() noexcept;

    /// One frame of the world: every playing burst advanced.
    [[nodiscard]] cy::Status step(cy::f32 dt) noexcept;
    /// The live sprites, camera-relative to `eye`, into `out`.
    [[nodiscard]] cy::Status publish(
        cy::Vec3 eye, cy::Array<cy::rendering::particles::ParticleInstance>& out) const noexcept;

    [[nodiscard]] cy::vfx::SimulationWorld& world() noexcept { return world_; }
    [[nodiscard]] const cy::vfx::CompiledSystem& muzzle() const noexcept { return *muzzle_; }
    [[nodiscard]] const cy::vfx::CompiledSystem& sparks() const noexcept { return *sparks_; }
    [[nodiscard]] cy::rendering::DecalBudget& decals() noexcept { return decals_; }
    [[nodiscard]] const cy::rendering::DecalBudget& decals() const noexcept { return decals_; }
    /// The decal materials, indexed by `DecalInstance::material_index`: the scorch is 0.
    [[nodiscard]] cy::Span<const cy::rendering::decals::DecalMaterial> decal_materials()
        const noexcept {
        return {decal_materials_.data(), decal_materials_.size()};
    }
    [[nodiscard]] const cy::vfx::StepReport& last_step() const noexcept { return stepped_; }
    /// Particles that live through a frame of the world since the run began, summed: what the
    /// report says the effects did.
    [[nodiscard]] cy::u64 particle_frames() const noexcept { return particle_frames_; }
    [[nodiscard]] cy::u32 peak_particles() const noexcept { return peak_particles_; }

    /// How long a play spawns for.
    static constexpr cy::f32 kMuzzleSeconds = 0.05F;
    static constexpr cy::f32 kSparkSeconds = 0.05F;

private:
    cy::Allocator* allocator_;
    cy::graph::DiagnosticSink sink_;
    std::unique_ptr<cy::vfx::CompiledSystem> muzzle_;
    std::unique_ptr<cy::vfx::CompiledSystem> sparks_;
    cy::vfx::SimulationWorld world_;
    cy::vfx::StepReport stepped_;
    cy::rendering::DecalBudget decals_;
    std::vector<cy::rendering::decals::DecalMaterial> decal_materials_;
    cy::u64 particle_frames_ = 0;
    cy::u32 peak_particles_ = 0;
};

}  // namespace sample::fps

#endif  // CY_SAMPLE_FIRST_PERSON_SHOT_EFFECTS_H
