#include <cy/rendering/lighting/lights.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering {
namespace {

[[nodiscard]] u32 gpu_kind_of(render::LightKind kind) noexcept {
    switch (kind) {
        case render::LightKind::Directional:
            return kGpuLightDirectional;
        case render::LightKind::Point:
            return kGpuLightPoint;
        case render::LightKind::Spot:
            return kGpuLightSpot;
        case render::LightKind::Count:
            break;
    }
    return kGpuLightPoint;
}

/// The smooth cone attenuation, as `saturate(cos(angle) * scale + bias)`.
///
/// The guard on the denominator is not defensive noise: inner == outer is a hard-edged spot, which
/// is a perfectly reasonable thing to author, and it is exactly the case that divides by zero.
void spot_terms(f32 inner_radians, f32 outer_radians, f32& scale, f32& bias) noexcept {
    const f32 outer = math::clamp(outer_radians, 0.0F, kPi * 0.5F);
    const f32 inner = math::clamp(inner_radians, 0.0F, outer);
    const f32 cos_outer = std::cos(outer);
    const f32 cos_inner = std::cos(inner);
    scale = 1.0F / math::max(cos_inner - cos_outer, 1e-4F);
    bias = -cos_outer * scale;
}

}  // namespace

GpuLight build_gpu_light(const render::LightDescription& light,
                         const LightBuildParameters& parameters) noexcept {
    GpuLight record;
    record.kind = gpu_kind_of(light.kind);

    // THE CAMERA-RELATIVE SUBTRACTION, in f64. Both operands are exact as doubles at any world
    // coordinate a game uses, so the difference is exact and only the RESULT — a small number — is
    // rounded to f32. Subtracting in f32 would round both operands first, which is precisely the
    // error a million units from the origin.
    const auto light_x = static_cast<f64>(light.transform.translation.x);
    const auto light_y = static_cast<f64>(light.transform.translation.y);
    const auto light_z = static_cast<f64>(light.transform.translation.z);
    record.position_relative_to_camera[0] =
        static_cast<f32>(light_x - static_cast<f64>(parameters.camera_origin.x));
    record.position_relative_to_camera[1] =
        static_cast<f32>(light_y - static_cast<f64>(parameters.camera_origin.y));
    record.position_relative_to_camera[2] =
        static_cast<f32>(light_z - static_cast<f64>(parameters.camera_origin.z));

    const Vec3 direction = normalize(light.transform.forward());
    record.direction[0] = direction.x;
    record.direction[1] = direction.y;
    record.direction[2] = direction.z;

    record.range = light.kind == render::LightKind::Directional ? 0.0F : light.range;
    record.intensity = to_shading_intensity(light.kind, parameters.unit, light.intensity,
                                            light.outer_cone_radians, 0.0F);

    // The temperature is a TINT: unit luminance, so it multiplies the authored colour without
    // changing how bright the light is. units.h is where that normalisation lives.
    Vec3 color{light.color[0], light.color[1], light.color[2]};
    if (parameters.temperature_kelvin > 0.0F) {
        const Vec3 tint = blackbody_color(parameters.temperature_kelvin);
        color = Vec3{color.x * tint.x, color.y * tint.y, color.z * tint.z};
    }
    record.color[0] = color.x;
    record.color[1] = color.y;
    record.color[2] = color.z;

    if (light.kind == render::LightKind::Spot) {
        spot_terms(light.inner_cone_radians, light.outer_cone_radians, record.spot_scale,
                   record.spot_bias);
    } else {
        // A non-spot's cone must evaluate to 1 for every direction, and `scale = 0, bias = 1` is
        // the pair that does it without the shader branching on the kind twice.
        record.spot_scale = 0.0F;
        record.spot_bias = 1.0F;
    }

    record.shadow_slot = kNoShadowSlot;
    record.layer_mask = light.layer_mask;
    return record;
}

f32 light_importance(const render::LightDescription& light, Vec3 camera_position,
                     f32 shading_intensity) noexcept {
    if (light.kind == render::LightKind::Directional) {
        // A directional light covers the whole view, so its coverage term is 1 and its importance
        // is its intensity. It is never the light that gets dropped, which is the right answer.
        return shading_intensity;
    }
    const f32 radius = light_bounding_radius(light);
    const Vec3 offset = light.transform.translation - camera_position;
    const f32 distance = length(offset);
    // Solid-angle coverage, approximated by the ratio of radius to distance. The camera inside the
    // light is the case that must not divide by zero and must rank highest, which max() gives.
    const f32 coverage = radius / math::max(distance, radius);
    return coverage * shading_intensity;
}

f32 light_bounding_radius(const render::LightDescription& light) noexcept {
    if (light.kind == render::LightKind::Directional) {
        return math::kInfinity;
    }
    return math::max(light.range, 0.0F);
}

}  // namespace cy::rendering
