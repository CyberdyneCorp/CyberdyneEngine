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
        case render::LightKind::Rect:
            return kGpuLightRect;
        case render::LightKind::Disc:
            return kGpuLightDisc;
        case render::LightKind::Sphere:
            return kGpuLightSphere;
        case render::LightKind::Tube:
            return kGpuLightTube;
        case render::LightKind::Count:
            break;
    }
    return kGpuLightPoint;
}

/// An area light's shape words: the two half-extents where a spot keeps its cone, the emitting
/// normal (a tube's axis) in `direction`, the width axis in the shadow-slot word, and the two-sided
/// flag above the kind's byte. See `GpuLight`.
void write_area_shape(const render::LightDescription& light, f32 shading_luminance,
                      GpuLight& record) noexcept {
    const f32 width = math::max(light.source_width, 0.0F);
    const f32 height = math::max(light.source_height, 0.0F);
    const Vec3 right = normalize(light.transform.right());
    switch (light.kind) {
        case render::LightKind::Rect:
            record.spot_scale = width * 0.5F;
            record.spot_bias = height * 0.5F;
            break;
        case render::LightKind::Disc:
        case render::LightKind::Sphere:
            record.spot_scale = width * 0.5F;
            record.spot_bias = width * 0.5F;
            break;
        case render::LightKind::Tube:
            record.spot_scale = width * 0.5F;
            record.spot_bias = height * 0.5F;
            // A tube's one direction is its axis, which is the transform's right axis.
            record.direction[0] = right.x;
            record.direction[1] = right.y;
            record.direction[2] = right.z;
            break;
        case render::LightKind::Directional:
        case render::LightKind::Point:
        case render::LightKind::Spot:
        case render::LightKind::Count:
            break;
    }
    const bool faces =
        light.kind == render::LightKind::Rect || light.kind == render::LightKind::Disc;
    if (faces && light.two_sided) {
        record.kind |= kGpuLightTwoSided;
    }
    record.shadow_slot = encode_area_tangent(right);
    record.range = light.range > 0.0F ? light.range : area_light_reach(light, shading_luminance);
}

/// Octahedral folding of a unit vector onto the square [-1, 1]^2.
[[nodiscard]] f32 sign_not_zero(f32 value) noexcept {
    return value >= 0.0F ? 1.0F : -1.0F;
}

[[nodiscard]] u32 snorm16(f32 value) noexcept {
    const f32 clamped = math::clamp(value, -1.0F, 1.0F);
    const auto integer = static_cast<i32>(std::lround(clamped * 32767.0F));
    return static_cast<u32>(integer) & 0xFFFFU;
}

[[nodiscard]] f32 from_snorm16(u32 bits) noexcept {
    const auto integer = static_cast<i32>(static_cast<i16>(static_cast<u16>(bits & 0xFFFFU)));
    return math::max(static_cast<f32>(integer) / 32767.0F, -1.0F);
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
    record.intensity =
        to_shading_intensity(light.kind, parameters.unit, light.intensity, light.outer_cone_radians,
                             area_light_emitting_area(light));

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

    if (light.kind == render::LightKind::Spot && light.cosine_falloff) {
        // THE COSINE ITSELF: `saturate(cos(angle))`, applied once rather than squared. A small
        // Lambertian emitter's intensity falls as the cosine from its normal, all the way to the
        // horizon; the authored cone is not consulted.
        record.kind = kGpuLightEmitter;
        record.spot_scale = 1.0F;
        record.spot_bias = 0.0F;
    } else if (light.kind == render::LightKind::Spot) {
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
    if (render::is_area_light(light.kind)) {
        write_area_shape(light, record.intensity, record);
    }
    return record;
}

GpuLight baked_light_placeholder() noexcept {
    GpuLight record;
    record.kind = kGpuLightBaked;
    record.intensity = 0.0F;
    record.range = 0.0F;
    record.layer_mask = 0U;
    record.shadow_slot = kNoShadowSlot;
    return record;
}

f32 area_light_reach(const render::LightDescription& light, f32 shading_luminance) noexcept {
    const f32 area = area_light_emitting_area(light);
    const f32 extent = 0.5F * math::max(light.source_width, light.source_height);
    const f32 luminance = math::max(shading_luminance, 0.0F) *
                          math::max(math::max(light.color[0], light.color[1]), light.color[2]);
    const f32 far_field = std::sqrt(luminance * area / kAreaLightReachIlluminance);
    return math::max(far_field, extent * 2.0F);
}

u32 encode_area_tangent(Vec3 tangent) noexcept {
    const Vec3 unit = normalized_or(tangent, Vec3{1.0F, 0.0F, 0.0F});
    const f32 sum = std::fabs(unit.x) + std::fabs(unit.y) + std::fabs(unit.z);
    f32 x = unit.x / sum;
    f32 y = unit.y / sum;
    if (unit.z < 0.0F) {
        const f32 folded_x = (1.0F - std::fabs(y)) * sign_not_zero(x);
        const f32 folded_y = (1.0F - std::fabs(x)) * sign_not_zero(y);
        x = folded_x;
        y = folded_y;
    }
    return snorm16(x) | (snorm16(y) << 16U);
}

Vec3 decode_area_tangent(u32 word) noexcept {
    const f32 x = from_snorm16(word);
    const f32 y = from_snorm16(word >> 16U);
    Vec3 unit{x, y, 1.0F - std::fabs(x) - std::fabs(y)};
    if (unit.z < 0.0F) {
        const f32 folded_x = (1.0F - std::fabs(unit.y)) * sign_not_zero(unit.x);
        const f32 folded_y = (1.0F - std::fabs(unit.x)) * sign_not_zero(unit.y);
        unit.x = folded_x;
        unit.y = folded_y;
    }
    return normalized_or(unit, Vec3{1.0F, 0.0F, 0.0F});
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
    if (render::is_area_light(light.kind) && !(light.range > 0.0F)) {
        const f32 luminance =
            to_shading_intensity(light.kind, default_unit_for(light.kind), light.intensity,
                                 light.outer_cone_radians, area_light_emitting_area(light));
        return area_light_reach(light, luminance);
    }
    return math::max(light.range, 0.0F);
}

}  // namespace cy::rendering
