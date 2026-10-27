// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_FIRST_PERSON_ILLUMINATION_H
#define CY_SAMPLE_FIRST_PERSON_ILLUMINATION_H
// illumination.h — the arena's global illumination: one `gi::IlluminationSystem` over the same
// boxes the physics world and the renderer read (arena.h), the props as movable objects moved from
// their bodies, the sun in the bake's units and the lamp strip's glow carried by the surface cache.
//
// It runs once a frame after the game's frame, from the eye — presentation, which the simulation
// never reads — and the renderer lights its frame with it on the device (`gi_gpu::GpuGiFrame`).
// The muzzle flashes the game spawns light the forward frame (ABI 1.11) and are not handed to the
// GI: a light that lives fifty milliseconds is gone before the GI's caches would have converged on
// it. samples/13-rts-api/host/illumination.h is the same arrangement for the RTS.

#include <cy/core/base/expected.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/allocator.h>
#include <cy/rendering/gi/mesh_cards.h>
#include <cy/rendering/gi/system.h>

#include <vector>

#include "arena.h"

namespace sample::fps {

class ArenaIllumination {
public:
    explicit ArenaIllumination(cy::Allocator& allocator) noexcept;

    /// Configure the system and hand it the arena's boxes, its props and the sun.
    [[nodiscard]] cy::Status build() noexcept;
    /// One update from `eye`, the props where their bodies are.
    [[nodiscard]] cy::Status update(cy::Vec3 eye, const cy::Transform (&props)[kPropCount],
                                    cy::u64 frame) noexcept;

    [[nodiscard]] cy::rendering::gi::IlluminationSystem& system() noexcept { return system_; }
    [[nodiscard]] cy::u64 frame() const noexcept { return frame_; }
    [[nodiscard]] cy::f64 last_update_ms() const noexcept { return update_ms_; }

private:
    cy::Allocator* allocator_;
    cy::rendering::gi::IlluminationSystem system_;
    cy::rendering::gi::MeshDistanceField cube_field_;
    std::vector<cy::rendering::gi::GiLight> lights_;
    cy::u64 frame_ = 0;
    cy::f64 update_ms_ = 0.0;
    bool built_ = false;
};

}  // namespace sample::fps

#endif  // CY_SAMPLE_FIRST_PERSON_ILLUMINATION_H
