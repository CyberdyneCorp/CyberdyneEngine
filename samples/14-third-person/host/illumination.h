// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_THIRD_PERSON_ILLUMINATION_H
#define CY_SAMPLE_THIRD_PERSON_ILLUMINATION_H
// illumination.h — the level's global illumination: the courtyard as boxes, for CyberGI.
//
// The same arrangement samples/13-rts-api/host/illumination.h makes for its level: every object
// the GI should see bounce light off is a box of one `gi::IlluminationSystem` — cards over its
// faces and a distance grid placed by its transform — and the system's update runs once a frame
// from the hero, whom the camera frames. Here the boxes are the courtyard's flagstones, the
// stairs, the platform, its walls and pillars, the ramp, the arch, and a box round every building
// and rock the map places. The sun is the map's, in the bake's units.
//
// The renderer (render.h) lights its frame with this system on the device (`gi_gpu::GpuGiFrame`).
// Headless, the GI is not built: the game reads no light.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/rendering/gi/mesh_cards.h>
#include <cy/rendering/gi/system.h>

#include <vector>

namespace sample::tps {

/// One box as the GI sees it, in world metres.
struct GiBox {
    cy::Vec3 centre{0.0F, 0.0F, 0.0F};
    cy::Vec3 half{0.5F, 0.5F, 0.5F};
    cy::Vec3 albedo{0.5F, 0.5F, 0.5F};
    /// Cards this far apart: a metre for the ground, half a metre for everything else.
    cy::f32 card_spacing = 0.5F;
};

class TpsIllumination {
public:
    explicit TpsIllumination(cy::Allocator& allocator) noexcept;

    TpsIllumination(const TpsIllumination&) = delete;
    TpsIllumination& operator=(const TpsIllumination&) = delete;

    /// Configure the system and hand it `boxes` and the sun: the direction its light travels and
    /// its illuminance in lux.
    [[nodiscard]] cy::Status build(const std::vector<GiBox>& boxes, cy::Vec3 sun_travel,
                                   cy::f32 sun_lux) noexcept;
    /// One update of the GI from `focus`, which its clipmaps follow.
    [[nodiscard]] cy::Status update(cy::Vec3 focus, cy::u64 frame) noexcept;

    [[nodiscard]] bool built() const noexcept { return built_; }
    [[nodiscard]] cy::rendering::gi::IlluminationSystem& system() noexcept { return system_; }
    [[nodiscard]] cy::f64 last_update_ms() const noexcept { return update_ms_; }
    /// The frame the last update was stamped with: what the device GI's frame is prepared for.
    [[nodiscard]] cy::u64 frame() const noexcept { return frame_; }

private:
    cy::Allocator* allocator_;
    cy::rendering::gi::IlluminationSystem system_;
    cy::rendering::gi::MeshDistanceField cube_field_;
    std::vector<cy::rendering::gi::GiLight> lights_;
    cy::f64 update_ms_ = 0.0;
    cy::u64 frame_ = 0;
    bool built_ = false;
};

}  // namespace sample::tps

#endif  // CY_SAMPLE_THIRD_PERSON_ILLUMINATION_H
