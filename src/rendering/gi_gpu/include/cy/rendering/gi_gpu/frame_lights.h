// SPDX-License-Identifier: MIT
#pragma once
// The GI's promoted emitters as the forward frame's own lights. Issue #35.
//
// ================================================================================================
// THE GAP THIS CLOSES
// ================================================================================================
//
// A small, bright emitter is PROMOTED (gi/emissive.h): its emission leaves the surface cache and
// it becomes a `GiLight` every consumer of `shaded_direct` integrates exactly — the cards it
// lights, and so everything their bounce lights. But the light it throws DIRECTLY on the surfaces
// the frame draws is direct light, and the frame's GI composite is indirect only: with the lamp's
// emission out of the cache, nothing put that direct light on the frame's pixels, and a promoted
// lamp lit the floor under it only through the bounce. These are the frame's own lights for them,
// shaded with everything else the frame's clustered lights shade.
//
// ================================================================================================
// THE SAME LIGHT, IN THE FRAME'S TERMS
// ================================================================================================
//
// A promoted card is a one-sided Lambertian square of area `A` radiating `L = colour *
// intensity`. Seen from a receiver well beyond its size it is a point whose intensity is `L * A *
// cos(theta)` in the direction `theta` off its normal — exactly what a spot with
// `render::LightDescription::cosine_falloff` and a right-angled cone draws when its intensity is
// given as the emitter's flux, `pi * L * A` lumens (`spot_candela_from_lumens` is lumens over pi).
// The frame then shades a receiver with `albedo / pi * E`, the GI a card with `albedo * L * F`
// (`emitter_form_factor`), and the two agree wherever the square is small against its distance;
// `render.gi_gpu_frame_lights` measures how close at the distances a lamp lights a room.
//
// The light's reach is the emitter's own influence radius (`emitter_influence_radius`) at the
// threshold the GI invalidates with, so the frame lights what the GI's invalidation covers.

#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/lighting.h>
#include <cy/rendering/gi/system.h>
#include <cy/servers/render/model.h>

namespace cy::rendering::gi_gpu {

struct PromotedLightOptions {
    /// Whether the frame shadows them: a lamp under a table should not light the floor through it.
    bool casts_shadow = true;
    /// The irradiance below which a light's reach ends. Zero takes the system's own invalidation
    /// threshold (`EmissiveSettings::invalidation_irradiance`).
    f32 reach_irradiance = 0.0F;
};

/// One promoted emitter (`GiLight::emitter_area` positive) as a light of the frame. A light that is
/// not an emitter is returned as a point light of its own candela, for completeness.
[[nodiscard]] render::LightDescription promoted_frame_light(const gi::GiLight& emitter,
                                                            f32 reach_irradiance,
                                                            bool casts_shadow = true) noexcept;

/// Every emitter `system` promoted (`IlluminationSystem::emitters().lights()`), appended to `out`
/// as lights of the frame. Returns how many it appended. Call once a frame, after the system's
/// update, and hand `out` to the frame with its other lights.
u32 append_promoted_frame_lights(const gi::IlluminationSystem& system,
                                 Array<render::LightDescription>& out,
                                 const PromotedLightOptions& options = {}) noexcept;

}  // namespace cy::rendering::gi_gpu
