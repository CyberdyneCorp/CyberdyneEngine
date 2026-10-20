// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_API_ILLUMINATION_H
#define CY_SAMPLE_RTS_API_ILLUMINATION_H
// illumination.h — the level's global illumination, and ABI 1.9 over it. Issue #35, the live half.
//
// ================================================================================================
// WHAT THE HOST HANDS THE GI, AND WHAT IT LEAVES TO THE GAME
// ================================================================================================
//
// The level as the GI sees it is four boxes in the level's own metres (`field_boxes`): the 32 m
// ground, the barracks behind the spawn point, a beacon beside it whose glass glows (material
// "beacon"), and the crate the scout kicks. Each is an object of one `gi::IlluminationSystem` —
// cards over its faces and a distance grid placed by its transform — and the crate is MOVABLE: the
// host moves it every frame from its body, and its class prices the move. The sun is the
// frame's, in the bake's units.
//
// The game decides everything else, through ABI 1.9 (`cy::game_backend::IlluminationAdapter`,
// bound here): the commander places a GI volume over its base, reads the light where its units
// stand, and puts the beacon out and lights it again by its material's name (game/Lighting.swift).
// The host only runs the system's update, once a frame, from the ground point the camera looks at —
// presentation, as the ABI says: a fixed step that asked would be refused.
//
// The renderer (render.h), when the run has one, lights its frame with this same system on the
// device (`gi_gpu::GpuGiFrame`); headless, the GI still runs, so what the game reads is the same
// with or without a picture.

#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/game_backend/illumination_backend.h>
#include <cy/rendering/gi/mesh_cards.h>
#include <cy/rendering/gi/system.h>

#include <memory>
#include <vector>

namespace sample::rts {

/// One box of the level as the GI and the renderer both see it, in the level's metres.
struct FieldBox {
    const char* material = "";
    cy::Vec3 centre{0.0F, 0.0F, 0.0F};
    cy::Vec3 half{0.5F, 0.5F, 0.5F};
    cy::Vec3 albedo{0.5F, 0.5F, 0.5F};
    /// Radiance, for a glowing box.
    cy::Vec3 emission{0.0F, 0.0F, 0.0F};
    /// Cards a metre apart for the ground, half a metre for everything else.
    cy::f32 card_spacing = 0.5F;
    bool movable = false;
};

/// The ground, the barracks, the beacon and the crate, in that order.
[[nodiscard]] cy::Span<const FieldBox> field_boxes() noexcept;
inline constexpr cy::u32 kGroundBox = 0;
inline constexpr cy::u32 kCrateBox = 3;
/// The material the beacon's glass is drawn with: what the game names it by.
inline constexpr const char* kBeaconMaterial = "beacon";
/// The sun: the direction its light travels, and its illuminance in lux.
inline constexpr cy::Vec3 kSunTravel{-0.28F, -0.82F, -0.50F};
inline constexpr cy::f32 kSunLux = 22000.0F;

class RtsIllumination {
public:
    explicit RtsIllumination(cy::Allocator& allocator) noexcept;
    ~RtsIllumination();

    RtsIllumination(const RtsIllumination&) = delete;
    RtsIllumination& operator=(const RtsIllumination&) = delete;

    /// Configure the system and hand it the level's boxes and the sun.
    [[nodiscard]] cy::Status build() noexcept;
    /// Bind ABI 1.9 on `host`, the crate named by its entity.
    [[nodiscard]] cy::Status bind(cy::abi::Host& host, CyEntity crate) noexcept;
    void unbind(cy::abi::Host& host) noexcept;

    /// Before the frame's scripts: the frame their changes are stamped with.
    void begin_frame(cy::u64 frame) noexcept;
    /// After them: the crate where its body is, and one update of the GI from `focus`, the ground
    /// point the camera looks at, which the GI's clipmaps follow.
    [[nodiscard]] cy::Status update(cy::Vec3 focus, cy::Vec3 crate, cy::u64 frame) noexcept;

    [[nodiscard]] cy::rendering::gi::IlluminationSystem& system() noexcept { return system_; }
    [[nodiscard]] cy::u64 frame() const noexcept { return frame_; }
    /// The wall time of the last `update`: the GI's host half — card shading and the probe
    /// gather on the processor — which a frame pays besides the device's.
    [[nodiscard]] cy::f64 last_update_ms() const noexcept { return update_ms_; }
    [[nodiscard]] cy::game_backend::IlluminationAdapter* adapter() noexcept {
        return adapter_.get();
    }

private:
    cy::Allocator* allocator_;
    cy::rendering::gi::IlluminationSystem system_;
    cy::rendering::gi::MeshDistanceField cube_field_;
    std::vector<cy::rendering::gi::GiLight> lights_;
    std::unique_ptr<cy::game_backend::IlluminationAdapter> adapter_;
    cy::u64 frame_ = 0;
    cy::f64 update_ms_ = 0.0;
    bool built_ = false;
};

}  // namespace sample::rts

#endif  // CY_SAMPLE_RTS_API_ILLUMINATION_H
