#include <cy/rendering/lighting/area_shading.h>

#include <cy/core/math/math.h>

#include <cmath>

namespace cy::rendering {
namespace {

[[nodiscard]] Vec3 position_of(const GpuLight& light) noexcept {
    return Vec3{light.position_relative_to_camera[0], light.position_relative_to_camera[1],
                light.position_relative_to_camera[2]};
}

[[nodiscard]] Vec3 direction_of(const GpuLight& light) noexcept {
    return normalized_or(Vec3{light.direction[0], light.direction[1], light.direction[2]},
                         Vec3{0.0F, -1.0F, 0.0F});
}

[[nodiscard]] Vec3 color_of(const GpuLight& light) noexcept {
    return Vec3{light.color[0], light.color[1], light.color[2]};
}

[[nodiscard]] AreaLightShape shape_of(u32 kind) noexcept {
    switch (kind & kGpuLightKindMask) {
        case kGpuLightDisc:
            return AreaLightShape::Disc;
        case kGpuLightSphere:
            return AreaLightShape::Sphere;
        case kGpuLightTube:
            return AreaLightShape::Tube;
        default:
            return AreaLightShape::Rect;
    }
}

/// A unit vector perpendicular to `axis`, preferring the one also perpendicular to `towards`.
[[nodiscard]] Vec3 facing_perpendicular(Vec3 axis, Vec3 towards) noexcept {
    const Vec3 fallback =
        std::fabs(axis.y) < 0.99F ? Vec3{0.0F, 1.0F, 0.0F} : Vec3{1.0F, 0.0F, 0.0F};
    return normalized_or(cross(axis, towards), normalize(cross(axis, fallback)));
}

/// `cy/light.slang`'s `distanceAttenuation`: inverse square, windowed to the range.
[[nodiscard]] f32 punctual_attenuation(f32 distance_squared, f32 range) noexcept {
    const f32 factor = distance_squared / math::max(range * range, 1.0e-6F);
    const f32 window = math::clamp(1.0F - (factor * factor), 0.0F, 1.0F);
    return (window * window) / math::max(distance_squared, 1.0e-4F);
}

/// `ltc_evaluate`'s single-sided rule: a one-sided emitter seen from behind lights nothing.
[[nodiscard]] bool area_light_lit(const AreaLight& light) noexcept {
    if (!light.single_sided || light.shape == AreaLightShape::Sphere ||
        light.shape == AreaLightShape::Tube) {
        return true;
    }
    return dot(cross(light.tangent_x, light.tangent_y), light.center) <= 0.0F;
}

/// A direction of the cosine lobe's space back into the GGX lobe's: `M = Minv^-1`, for the sparse
/// `Minv = [a 0 b; 0 c 0; d 0 1]` area.cpp fits, in closed form.
[[nodiscard]] Vec3 lobe_direction(const Mat3& minv, Vec3 direction) noexcept {
    const f32 a = minv.columns[0].x;
    const f32 d = minv.columns[0].z;
    const f32 c = minv.columns[1].y;
    const f32 b = minv.columns[2].x;
    const f32 determinant = (a - (b * d));
    if (std::fabs(determinant) < 1.0e-8F || std::fabs(c) < 1.0e-8F) {
        return direction;
    }
    return normalized_or(Vec3{(direction.x - (b * direction.z)) / determinant, direction.y / c,
                              ((a * direction.z) - (d * direction.x)) / determinant},
                         direction);
}

}  // namespace

AreaLight area_light_from_record(const GpuLight& light, Vec3 position) noexcept {
    AreaLight area;
    area.shape = shape_of(light.kind);
    area.center = position_of(light) - position;
    area.half_x = light.spot_scale;
    area.half_y = light.spot_bias;
    area.single_sided = (light.kind & kGpuLightTwoSided) == 0U;
    const Vec3 normal = direction_of(light);
    switch (area.shape) {
        case AreaLightShape::Rect:
        case AreaLightShape::Disc: {
            // `cross(tangent_x, tangent_y)` is the emitting normal, which is what area.h's
            // single-sided test reads: `tangent_y = normal x tangent_x` makes it so.
            const Vec3 tangent = decode_area_tangent(light.shadow_slot);
            area.tangent_x = normalized_or(tangent - normal * dot(tangent, normal),
                                           facing_perpendicular(normal, area.center));
            area.tangent_y = cross(normal, area.tangent_x);
            break;
        }
        case AreaLightShape::Tube: {
            // area.h's tube is `half_x` across and `half_y` along `tangent_y`; the record carries
            // the half LENGTH first, so the two are swapped here, and the quad is turned about the
            // axis to face the point — the silhouette a cylinder presents from the side.
            area.half_x = light.spot_bias;
            area.half_y = light.spot_scale;
            area.tangent_y = normal;
            area.tangent_x = facing_perpendicular(normal, area.center);
            area.single_sided = false;
            break;
        }
        case AreaLightShape::Sphere:
        case AreaLightShape::Count:
            area.single_sided = false;
            break;
    }
    return area;
}

f32 area_light_window(const GpuLight& light, Vec3 position) noexcept {
    const Vec3 offset = position_of(light) - position;
    const f32 factor = length_squared(offset) / math::max(light.range * light.range, 1.0e-6F);
    const f32 window = math::clamp(1.0F - (factor * factor), 0.0F, 1.0F);
    return window * window;
}

AreaShading shade_area_light(const LtcTable& table, const GpuLight& light, Vec3 position,
                             Vec3 normal, Vec3 view, const AreaSurface& surface) noexcept {
    AreaShading result;
    const f32 window = area_light_window(light, position);
    if (window <= 0.0F) {
        return result;
    }
    const AreaLight area = area_light_from_record(light, position);
    const Vec3 radiance = color_of(light) * (light.intensity * window);

    result.form_factor = ltc_evaluate_diffuse(area, normal);
    const Vec3 diffuse_albedo = surface.albedo * (1.0F - surface.metallic);
    result.diffuse = Vec3{diffuse_albedo.x * radiance.x, diffuse_albedo.y * radiance.y,
                          diffuse_albedo.z * radiance.z} *
                     result.form_factor;
    if (!table.built()) {
        return result;
    }

    // The specular: the LTC's polygon integral, its magnitude, and Fresnel where the lobe gathers
    // the light from (header comment).
    if (!area_light_lit(area)) {
        return result;
    }
    const f32 cos_theta = math::clamp(dot(normal, view), 1.0e-3F, 1.0F);
    const Vec3 tangent = normalized_or(view - normal * cos_theta, Vec3{1.0F, 0.0F, 0.0F});
    const Vec3 bitangent = cross(normal, tangent);
    const LtcEntry entry = table.sample(surface.roughness, cos_theta);
    const Mat3& m = entry.inverse_transform;
    const AreaQuad quad = area_light_quad(area);
    Vec3 local[4];
    for (u32 index = 0; index < 4; ++index) {
        const Vec3 corner = quad.corner[index];
        local[index] = m * Vec3{dot(corner, tangent), dot(corner, bitangent), dot(corner, normal)};
    }
    const PolygonIntegral polygon = integrate_cosine_polygon_vector(local, 4);
    const Vec3 gathered = lobe_direction(m, polygon.direction);
    const Vec3 direction =
        normalized_or(tangent * gathered.x + bitangent * gathered.y + normal * gathered.z, normal);
    const Vec3 half = normalized_or(view + direction, normal);
    const f32 one_minus = 1.0F - math::clamp(dot(view, half), 0.0F, 1.0F);
    const f32 schlick = one_minus * one_minus * one_minus * one_minus * one_minus;
    const Vec3 f0 = lerp(Vec3{0.04F, 0.04F, 0.04F}, surface.albedo, surface.metallic);
    const Vec3 fresnel{f0.x + ((1.0F - f0.x) * schlick), f0.y + ((1.0F - f0.y) * schlick),
                       f0.z + ((1.0F - f0.z) * schlick)};
    const f32 energy = polygon.integral * entry.magnitude;
    result.specular =
        Vec3{fresnel.x * radiance.x, fresnel.y * radiance.y, fresnel.z * radiance.z} * energy;
    return result;
}

Vec3 light_irradiance(const GpuLight& light, Vec3 position, Vec3 normal) noexcept {
    const u32 kind = light.kind & kGpuLightKindMask;
    if (kind == kGpuLightDirectional) {
        return Vec3{0.0F, 0.0F, 0.0F};
    }
    if (gpu_light_is_area(light.kind)) {
        const f32 window = area_light_window(light, position);
        const f32 form_factor =
            ltc_evaluate_diffuse(area_light_from_record(light, position), normal);
        return color_of(light) * (light.intensity * math::kPi * form_factor * window);
    }
    const Vec3 to_light = position_of(light) - position;
    const f32 distance_squared = length_squared(to_light);
    const Vec3 direction = to_light * (1.0F / std::sqrt(math::max(distance_squared, 1.0e-8F)));
    f32 attenuation = punctual_attenuation(distance_squared, light.range);
    const f32 cosine = dot(direction * -1.0F, direction_of(light));
    if (kind == kGpuLightSpot) {
        const f32 cone = math::clamp((cosine * light.spot_scale) + light.spot_bias, 0.0F, 1.0F);
        attenuation *= cone * cone;
    } else if (kind == kGpuLightEmitter) {
        attenuation *= math::clamp((cosine * light.spot_scale) + light.spot_bias, 0.0F, 1.0F);
    }
    const f32 lambert = math::clamp(dot(normal, direction), 0.0F, 1.0F);
    return color_of(light) * (light.intensity * attenuation * lambert);
}

Vec3 sample_light_point(const GpuLight& light, Vec3 towards, f32 u, f32 v) noexcept {
    const Vec3 center = position_of(light);
    if (!gpu_light_is_area(light.kind)) {
        return center;
    }
    const AreaLight area = area_light_from_record(light, towards);
    const f32 su = (2.0F * u) - 1.0F;
    const f32 sv = (2.0F * v) - 1.0F;
    switch (area.shape) {
        case AreaLightShape::Rect:
            return center + area.tangent_x * (su * area.half_x) +
                   area.tangent_y * (sv * area.half_y);
        case AreaLightShape::Tube:
            return center + area.tangent_x * (su * area.half_x) +
                   area.tangent_y * (sv * area.half_y);
        case AreaLightShape::Disc:
        case AreaLightShape::Sphere: {
            // A disc, uniform in area: the sphere's is the silhouette disc facing the point.
            const f32 radius = area.half_x * std::sqrt(u);
            const f32 angle = 2.0F * math::kPi * v;
            Vec3 axis_x = area.tangent_x;
            Vec3 axis_y = area.tangent_y;
            if (area.shape == AreaLightShape::Sphere) {
                const Vec3 facing = normalized_or(towards - center, Vec3{0.0F, 0.0F, 1.0F});
                axis_x = facing_perpendicular(facing, Vec3{0.0F, 1.0F, 0.0F});
                axis_y = cross(facing, axis_x);
            }
            return center + axis_x * (radius * std::cos(angle)) +
                   axis_y * (radius * std::sin(angle));
        }
        case AreaLightShape::Count:
            break;
    }
    return center;
}

}  // namespace cy::rendering
