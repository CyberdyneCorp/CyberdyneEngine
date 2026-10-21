// SPDX-License-Identifier: MIT
#include <cy/rendering/gi_gpu/frame_lights.h>

#include <cy/core/math/quat.h>
#include <cy/rendering/gi/emissive.h>

#include <algorithm>
#include <numbers>

namespace cy::rendering::gi_gpu {

render::LightDescription promoted_frame_light(const gi::GiLight& emitter, f32 reach_irradiance,
                                              bool casts_shadow) noexcept {
    render::LightDescription light;
    light.transform.translation = emitter.position;
    light.casts_shadow = casts_shadow;
    light.stable_id = emitter.id;
    // Colour at unit peak and the magnitude in the intensity, as the frame's lights carry them.
    const f32 peak = std::max({emitter.colour.x, emitter.colour.y, emitter.colour.z, 1.0e-12F});
    light.color[0] = emitter.colour.x / peak;
    light.color[1] = emitter.colour.y / peak;
    light.color[2] = emitter.colour.z / peak;
    light.range = gi::emitter_influence_radius(emitter, reach_irradiance);
    if (!(emitter.emitter_area > 0.0F)) {
        light.kind = render::LightKind::Point;
        light.intensity = emitter.intensity * peak;
        return light;
    }
    // A spot down the card's normal (the frame's forward is local -Z), a right-angled cone, and
    // the flux `pi * L * A` in lumens: the frame's spot convention divides it by pi again, so the
    // on-axis candela is `L * A`, falling as the cosine (`cosine_falloff`).
    light.kind = render::LightKind::Spot;
    light.cosine_falloff = true;
    light.transform.rotation =
        Quat::from_to(kAxisForward, normalized_or(emitter.emitter_normal, Vec3{0.0F, -1.0F, 0.0F}));
    light.inner_cone_radians = 0.0F;
    light.outer_cone_radians = std::numbers::pi_v<f32> * 0.5F;
    light.intensity = std::numbers::pi_v<f32> * emitter.intensity * peak * emitter.emitter_area;
    return light;
}

u32 append_promoted_frame_lights(const gi::IlluminationSystem& system,
                                 Array<render::LightDescription>& out,
                                 const PromotedLightOptions& options) noexcept {
    const f32 reach = options.reach_irradiance > 0.0F
                          ? options.reach_irradiance
                          : system.settings().emissive.invalidation_irradiance;
    u32 appended = 0;
    for (const gi::GiLight& emitter : system.emitters().lights()) {
        if (!out.push_back(promoted_frame_light(emitter, reach, options.casts_shadow))) {
            break;
        }
        appended += 1;
    }
    return appended;
}

}  // namespace cy::rendering::gi_gpu
