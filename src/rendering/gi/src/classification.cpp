// SPDX-License-Identifier: MIT
#include <cy/rendering/gi/classification.h>

#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>

namespace cy::rendering::gi {

const char* illumination_class_name(IlluminationClass value) noexcept {
    switch (value) {
        case IlluminationClass::Static:
            return "Static";
        case IlluminationClass::SlowDynamic:
            return "SlowDynamic";
        case IlluminationClass::FastDynamic:
            return "FastDynamic";
        case IlluminationClass::TransientIgnored:
            return "TransientIgnored";
        case IlluminationClass::Count:
            break;
    }
    return "Unknown";
}

IlluminationClass classify_illumination(ObjectMobility mobility, f32 size_metres,
                                        const ClassificationSettings& settings) noexcept {
    if (mobility == ObjectMobility::Static) {
        return IlluminationClass::Static;
    }
    if (size_metres < settings.transient_max_size_metres) {
        return IlluminationClass::TransientIgnored;
    }
    if (size_metres >= settings.slow_dynamic_min_size_metres) {
        return IlluminationClass::SlowDynamic;
    }
    return IlluminationClass::FastDynamic;
}

ProxyFidelity select_proxy_fidelity(f32 camera_distance_metres, f32 importance,
                                    const ClassificationSettings& settings) noexcept {
    const f32 reach = settings.deformed_proxy_distance_metres * std::max(importance, 0.0F);
    return camera_distance_metres <= reach ? ProxyFidelity::Deformed : ProxyFidelity::Coarse;
}

f32 sphere_occlusion(const Sphere& sphere, Vec3 position, Vec3 normal) noexcept {
    if (sphere.radius <= 0.0F) {
        return 0.0F;
    }
    const Vec3 offset = sphere.center - position;
    const f32 span = length(offset);
    if (span <= sphere.radius) {
        return 1.0F;
    }
    const f32 cosine = math::clamp(dot(normal, offset / span), -1.0F, 1.0F);
    const f32 ratio = span / sphere.radius;
    const f32 ratio2 = ratio * ratio;
    // Wholly above the horizon: the cosine-weighted solid angle of a sphere is (r/d)^2 cos(theta).
    // Wholly below it: nothing. Straddling it: Lagarde and de Rousiers' exact clipped form.
    const f32 straddle = 1.0F - (ratio2 * cosine * cosine);
    if (straddle <= 1.0e-3F) {
        return std::max(cosine, 0.0F) / ratio2;
    }
    const f32 sine2 = std::max(1.0F - (cosine * cosine), 1.0e-6F);
    const f32 outside = ratio2 - 1.0F;
    const f32 argument = math::clamp(-cosine * std::sqrt(outside / sine2), -1.0F, 1.0F);
    f32 clipped = (cosine * std::acos(argument)) - std::sqrt(straddle * outside);
    clipped = (clipped / ratio2) + std::atan(std::sqrt(straddle / outside));
    return math::clamp(clipped / math::kPi, 0.0F, 1.0F);
}

f32 sphere_hit_distance(const Sphere& sphere, Vec3 origin, Vec3 direction) noexcept {
    const Vec3 offset = origin - sphere.center;
    const f32 b = dot(offset, direction);
    const f32 c = dot(offset, offset) - (sphere.radius * sphere.radius);
    if (c > 0.0F && b > 0.0F) {
        return -1.0F;
    }
    const f32 discriminant = (b * b) - c;
    if (discriminant < 0.0F) {
        return -1.0F;
    }
    return std::max(-b - std::sqrt(discriminant), 0.0F);
}

f32 object_size_metres(const Aabb& local, const Mat4& transform) noexcept {
    if (local.is_empty()) {
        return 0.0F;
    }
    const Vec3 size = transformed(local, transform).size();
    return std::max({size.x, size.y, size.z});
}

}  // namespace cy::rendering::gi
