// SPDX-License-Identifier: MIT
#include <cy/rendering/gi/gi_volume.h>

#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cy::rendering::gi {
namespace {

[[nodiscard]] f32 distance_outside(const Aabb& box, Vec3 point) noexcept {
    const f32 dx = std::max({box.min.x - point.x, 0.0F, point.x - box.max.x});
    const f32 dy = std::max({box.min.y - point.y, 0.0F, point.y - box.max.y});
    const f32 dz = std::max({box.min.z - point.z, 0.0F, point.z - box.max.z});
    return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

[[nodiscard]] f32 lerp_toward(f32 value, f32 target, f32 weight) noexcept {
    return value + ((target - value) * weight);
}

[[nodiscard]] bool finite_positive(f32 value) noexcept {
    return std::isfinite(value) && value > 0.0F;
}

[[nodiscard]] bool finite_non_negative(f32 value) noexcept {
    return std::isfinite(value) && value >= 0.0F;
}

/// Fold one contributing volume into `settings`, parameter by parameter.
void apply(const GiVolume& volume, f32 weight, GiRegionSettings& settings) noexcept {
    if (volume.sets_parameter(GiVolumeParameter::Quality)) {
        settings.ray_scale =
            lerp_toward(settings.ray_scale, gi_quality_ray_scale(volume.quality), weight);
    }
    if (volume.sets_parameter(GiVolumeParameter::ProbeDensity)) {
        settings.probe_density = lerp_toward(settings.probe_density, volume.probe_density, weight);
    }
    if (volume.sets_parameter(GiVolumeParameter::UpdateRate)) {
        settings.update_rate = lerp_toward(settings.update_rate, volume.update_rate, weight);
    }
    if (volume.sets_parameter(GiVolumeParameter::DiffuseIntensity)) {
        settings.diffuse_intensity =
            lerp_toward(settings.diffuse_intensity, volume.diffuse_intensity, weight);
    }
    if (volume.sets_parameter(GiVolumeParameter::SpecularIntensity)) {
        settings.specular_intensity =
            lerp_toward(settings.specular_intensity, volume.specular_intensity, weight);
    }
    if (volume.sets_parameter(GiVolumeParameter::TierPreference) && weight >= 0.5F) {
        settings.tier = volume.tier;
    }
    if (volume.sets_parameter(GiVolumeParameter::FarFieldDistance)) {
        settings.far_field_begins_metres =
            lerp_toward(settings.far_field_begins_metres, volume.far_field_begins_metres, weight);
    }
}

}  // namespace

const char* gi_quality_name(GiQuality quality) noexcept {
    switch (quality) {
        case GiQuality::Low:
            return "Low";
        case GiQuality::Medium:
            return "Medium";
        case GiQuality::High:
            return "High";
        case GiQuality::Epic:
            return "Epic";
        case GiQuality::Count:
            break;
    }
    return "Unknown";
}

f32 gi_quality_ray_scale(GiQuality quality) noexcept {
    switch (quality) {
        case GiQuality::Low:
            return 0.5F;
        case GiQuality::High:
            return 2.0F;
        case GiQuality::Epic:
            return 4.0F;
        case GiQuality::Medium:
        case GiQuality::Count:
            break;
    }
    return 1.0F;
}

const char* gi_tier_preference_name(GiTierPreference preference) noexcept {
    switch (preference) {
        case GiTierPreference::Automatic:
            return "Automatic";
        case GiTierPreference::SoftwareOnly:
            return "SoftwareOnly";
        case GiTierPreference::WorldOnly:
            return "WorldOnly";
        case GiTierPreference::Count:
            break;
    }
    return "Unknown";
}

const char* gi_volume_parameter_name(GiVolumeParameter parameter) noexcept {
    switch (parameter) {
        case GiVolumeParameter::Quality:
            return "Quality";
        case GiVolumeParameter::ProbeDensity:
            return "ProbeDensity";
        case GiVolumeParameter::UpdateRate:
            return "UpdateRate";
        case GiVolumeParameter::DiffuseIntensity:
            return "DiffuseIntensity";
        case GiVolumeParameter::SpecularIntensity:
            return "SpecularIntensity";
        case GiVolumeParameter::TierPreference:
            return "TierPreference";
        case GiVolumeParameter::FarFieldDistance:
            return "FarFieldDistance";
        case GiVolumeParameter::Count:
            break;
    }
    return "Unknown";
}

Status validate_gi_volume(const GiVolume& volume) noexcept {
    const Aabb& box = volume.bounds;
    if (box.is_empty() || !std::isfinite(box.min.x) || !std::isfinite(box.max.x) ||
        !std::isfinite(box.min.y) || !std::isfinite(box.max.y) || !std::isfinite(box.min.z) ||
        !std::isfinite(box.max.z)) {
        return fail(ErrorCode::InvalidArgument, "GiVolume: the bounds are empty or not finite");
    }
    if (!finite_non_negative(volume.blend_distance_metres)) {
        return fail(ErrorCode::InvalidArgument, "GiVolume: the blend distance must be >= 0");
    }
    if (!finite_non_negative(volume.weight) || volume.weight > 1.0F) {
        return fail(ErrorCode::InvalidArgument, "GiVolume: the weight must be in [0, 1]");
    }
    if (!finite_positive(volume.probe_density) || !finite_positive(volume.update_rate) ||
        !finite_positive(volume.far_field_begins_metres)) {
        return fail(ErrorCode::InvalidArgument,
                    "GiVolume: the probe density, update rate and far-field distance must be > 0");
    }
    if (!finite_non_negative(volume.diffuse_intensity) ||
        !finite_non_negative(volume.specular_intensity)) {
        return fail(ErrorCode::InvalidArgument, "GiVolume: an intensity must be >= 0");
    }
    if (volume.quality >= GiQuality::Count || volume.tier >= GiTierPreference::Count ||
        (volume.sets >> static_cast<u32>(GiVolumeParameter::Count)) != 0U) {
        return fail(ErrorCode::InvalidArgument,
                    "GiVolume: the quality, tier preference or parameter mask is out of range");
    }
    return ok();
}

Status GiVolumeSet::assign(Span<const GiVolume> volumes) noexcept {
    for (const GiVolume& volume : volumes) {
        if (Status valid = validate_gi_volume(volume); !valid) {
            return valid;
        }
    }
    Array<GiVolume> ordered;
    if (Status appended = ordered.append(volumes); !appended) {
        return appended;
    }
    std::ranges::stable_sort(
        ordered, [](const GiVolume& a, const GiVolume& b) { return a.priority < b.priority; });
    volumes_ = std::move(ordered);
    revision_ += 1;
    return ok();
}

void GiVolumeSet::clear() noexcept {
    volumes_.clear();
    revision_ += 1;
}

f32 GiVolumeSet::contribution(const GiVolume& volume, Vec3 point) noexcept {
    const f32 outside = distance_outside(volume.bounds, point);
    f32 fade = 1.0F;
    if (outside > 0.0F) {
        fade = volume.blend_distance_metres > 0.0F
                   ? math::saturate(1.0F - (outside / volume.blend_distance_metres))
                   : 0.0F;
    }
    return volume.weight * fade;
}

GiRegionSettings GiVolumeSet::resolve(Vec3 point, const GiRegionSettings& global) const noexcept {
    GiRegionSettings settings = global;
    for (const GiVolume& volume : volumes_) {
        const f32 weight = contribution(volume, point);
        if (weight > 0.0F) {
            apply(volume, weight, settings);
        }
    }
    return settings;
}

}  // namespace cy::rendering::gi
