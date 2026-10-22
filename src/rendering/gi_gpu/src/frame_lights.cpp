// SPDX-License-Identifier: MIT
#include <cy/rendering/gi_gpu/frame_lights.h>

#include <cy/core/math/quat.h>
#include <cy/rendering/gi/emissive.h>
#include <cy/rendering/lighting/lights.h>
#include <cy/rendering/lighting/units.h>

#include <algorithm>
#include <cmath>
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

namespace {

constexpr f32 kPi = std::numbers::pi_v<f32>;

[[nodiscard]] gi::LightMobility gi_mobility(render::LightMobility mobility) noexcept {
    switch (mobility) {
        case render::LightMobility::Static:
            return gi::LightMobility::Static;
        case render::LightMobility::Stationary:
            return gi::LightMobility::Stationary;
        case render::LightMobility::Movable:
        case render::LightMobility::Count:
            break;
    }
    return gi::LightMobility::Movable;
}

/// One emitter square: `area` square metres centred on `centre`, facing `normal`.
[[nodiscard]] Status append_square(const gi::GiLight& prototype, Vec3 centre, Vec3 normal, f32 area,
                                   Array<gi::GiLight>& out, u32& appended) noexcept {
    gi::GiLight square = prototype;
    square.position = centre;
    square.emitter_normal = normalize(normal);
    square.emitter_area = area;
    square.direction = square.emitter_normal;
    if (Status added = out.push_back(square); !added) {
        return added;
    }
    ++appended;
    return ok();
}

/// A strip `length` long along `axis` and `width` across, facing `normal`, as squares of its width.
[[nodiscard]] Status append_strip(const gi::GiLight& prototype, Vec3 centre, Vec3 axis, Vec3 normal,
                                  f32 length, f32 width, Array<gi::GiLight>& out,
                                  u32& appended) noexcept {
    const f32 side = std::min(length, width);
    const u32 count =
        side > 0.0F ? std::clamp(static_cast<u32>(std::lround(length / side)), 1U, 16U) : 1U;
    const f32 step = length / static_cast<f32>(count);
    const f32 area = (length * width) / static_cast<f32>(count);
    for (u32 index = 0; index < count; ++index) {
        const f32 along = ((static_cast<f32>(index) + 0.5F) * step) - (0.5F * length);
        if (Status added =
                append_square(prototype, centre + axis * along, normal, area, out, appended);
            !added) {
            return added;
        }
    }
    return ok();
}

}  // namespace

u32 append_gi_lights(const render::LightDescription& light, Array<gi::GiLight>& out) noexcept {
    gi::GiLight prototype;
    prototype.colour = Vec3{light.color[0], light.color[1], light.color[2]};
    prototype.id = light.stable_id;
    prototype.mobility = gi_mobility(light.mobility);
    prototype.range = light.range;
    prototype.position = light.transform.translation;
    prototype.direction = normalized_or(light.transform.forward(), Vec3{0.0F, -1.0F, 0.0F});
    u32 appended = 0;
    if (!render::is_area_light(light.kind)) {
        prototype.directional = light.kind == render::LightKind::Directional;
        // The frame's shading quantity — lux, or candela from the authored lumens — over pi.
        prototype.intensity =
            to_shading_intensity(light.kind, default_unit_for(light.kind), light.intensity,
                                 light.outer_cone_radians, 0.0F) /
            kPi;
        if (prototype.directional) {
            prototype.range = 0.0F;
        }
        return out.push_back(prototype) ? 1U : 0U;
    }
    // An emitter radiates its luminance; the GI's form factor carries the pi.
    prototype.intensity =
        to_shading_intensity(light.kind, default_unit_for(light.kind), light.intensity,
                             light.outer_cone_radians, area_light_emitting_area(light));
    prototype.range = light_bounding_radius(light);
    const Vec3 centre = light.transform.translation;
    const Vec3 forward = prototype.direction;
    const Vec3 right = normalized_or(light.transform.right(), Vec3{1.0F, 0.0F, 0.0F});
    const Vec3 up = normalized_or(light.transform.up(), Vec3{0.0F, 1.0F, 0.0F});
    const f32 width = std::max(light.source_width, 0.0F);
    const f32 height = std::max(light.source_height, 0.0F);
    const u32 sides = light.two_sided ? 2U : 1U;
    Status status = ok();
    switch (light.kind) {
        case render::LightKind::Rect:
            for (u32 side = 0; side < sides && status; ++side) {
                const Vec3 facing = side == 0 ? forward : forward * -1.0F;
                status = width >= height ? append_strip(prototype, centre, right, facing, width,
                                                        height, out, appended)
                                         : append_strip(prototype, centre, up, facing, height,
                                                        width, out, appended);
            }
            break;
        case render::LightKind::Disc:
            for (u32 side = 0; side < sides && status; ++side) {
                status = append_square(prototype, centre, side == 0 ? forward : forward * -1.0F,
                                       kPi * 0.25F * width * width, out, appended);
            }
            break;
        case render::LightKind::Sphere: {
            // A SPHERE IS A POINT TO EVERY RECEIVER OUTSIDE IT: a Lambertian sphere of luminance L
            // presents `pi r^2` of it in every direction, so its irradiance is `L pi r^2 cos / d^2`
            // — a point of `L r^2` in the GI's units, exact, where no set of flat squares is (a
            // cube of its area is a third dim face-on). Its radius softens the GI's shadow of it.
            const f32 radius = 0.5F * width;
            gi::GiLight sphere = prototype;
            sphere.intensity = prototype.intensity * radius * radius;
            sphere.radius = radius;
            status = out.push_back(sphere);
            appended += status ? 1U : 0U;
            break;
        }
        case render::LightKind::Tube: {
            // Eight rows around the axis, each an eighth of the side's area: the side's projected
            // width is the prism's to within three per cent from every direction across the axis.
            constexpr u32 kRows = 8;
            const f32 radius = 0.5F * height;
            const f32 row_width = (2.0F * kPi * radius) / static_cast<f32>(kRows);
            for (u32 row = 0; row < kRows && status; ++row) {
                const f32 angle = (2.0F * kPi * static_cast<f32>(row)) / static_cast<f32>(kRows);
                const Vec3 normal = up * std::cos(angle) + forward * std::sin(angle);
                status = append_strip(prototype, centre + normal * radius, right, normal, width,
                                      row_width, out, appended);
            }
            break;
        }
        case render::LightKind::Directional:
        case render::LightKind::Point:
        case render::LightKind::Spot:
        case render::LightKind::Count:
            break;
    }
    return appended;
}

}  // namespace cy::rendering::gi_gpu
