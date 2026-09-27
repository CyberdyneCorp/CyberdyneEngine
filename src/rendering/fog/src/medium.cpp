// SPDX-License-Identifier: MIT
#include <cy/rendering/fog/medium.h>

#include <cy/core/math/scalar.h>
#include <cy/rendering/post/effects.h>

#include <cmath>

namespace cy::rendering::fog {
namespace {

/// The signed distance inward from a volume's boundary, metres: positive inside. Not a Euclidean
/// distance for the cone, whose slanted side is measured horizontally — the edge ramp is a few
/// froxels wide, and what it has to be is continuous and zero on the boundary.
[[nodiscard]] f32 inside_distance(const FogVolume& volume, Vec3 world) noexcept {
    const Vec3 local = world - volume.centre;
    const f32 radial = std::sqrt((local.x * local.x) + (local.z * local.z));
    switch (volume.shape) {
        case FogVolumeShape::Box:
            return math::min(volume.size.x - std::fabs(local.x),
                             math::min(volume.size.y - std::fabs(local.y),
                                       volume.size.z - std::fabs(local.z)));
        case FogVolumeShape::Sphere:
            return volume.size.x - length(local);
        case FogVolumeShape::Cylinder:
            return math::min(volume.size.x - radial, volume.size.y - std::fabs(local.y));
        case FogVolumeShape::Cone: {
            const f32 height = math::max(volume.size.y, 1e-6F);
            const f32 radius = volume.size.x * (1.0F - (local.y / height));
            return math::min(math::min(local.y, height - local.y), radius - radial);
        }
    }
    return -1.0F;
}

void add_medium(MediumSample& sample, f32 extinction, Vec3 albedo, f32 anisotropy,
                Vec3 emission, f32 cos_to_sun) noexcept {
    sample.emission += emission;
    if (extinction <= 0.0F) {
        return;
    }
    const Vec3 scattering = albedo * extinction;
    const f32 phase = henyey_greenstein(anisotropy, cos_to_sun);
    sample.extinction += extinction;
    sample.scattering += scattering;
    sample.sun_scattering += scattering * phase;
}

}  // namespace

f32 extinction_for_visibility(f32 metres) noexcept {
    return -std::log(kKoschmiederContrast) / math::max(metres, 1e-3F);
}

f32 height_fog_extinction(const HeightFog& fog, f32 altitude) noexcept {
    if (fog.extinction <= 0.0F) {
        return 0.0F;
    }
    const f32 above = math::max(altitude - fog.base_height, 0.0F);
    return fog.extinction * std::exp(-above / math::max(fog.scale_height, 1e-3F));
}

f32 fog_volume_coverage(const FogVolume& volume, Vec3 world) noexcept {
    const f32 inside = inside_distance(volume, world);
    if (inside <= 0.0F) {
        return 0.0F;
    }
    if (volume.edge <= 0.0F) {
        return 1.0F;
    }
    return math::saturate(inside / volume.edge);
}

MediumSample sample_medium(const FogMedium& medium, Vec3 world, f32 cos_to_sun) noexcept {
    MediumSample sample;
    add_medium(sample, height_fog_extinction(medium.height, world.y), medium.height.albedo,
               medium.height.anisotropy, Vec3{0.0F, 0.0F, 0.0F}, cos_to_sun);
    const u32 count = math::min(medium.volume_count, kMaxFogVolumes);
    for (u32 index = 0; index < count; ++index) {
        const FogVolume& volume = medium.volumes[index];
        const f32 coverage = fog_volume_coverage(volume, world);
        if (coverage <= 0.0F) {
            continue;
        }
        add_medium(sample, volume.extinction * coverage, volume.albedo, volume.anisotropy,
                   volume.emission * coverage, cos_to_sun);
    }
    return sample;
}

bool medium_is_empty(const FogMedium& medium) noexcept {
    if (medium.height.extinction > 0.0F) {
        return false;
    }
    const u32 count = math::min(medium.volume_count, kMaxFogVolumes);
    for (u32 index = 0; index < count; ++index) {
        const FogVolume& volume = medium.volumes[index];
        if (volume.extinction > 0.0F || max_component(volume.emission) > 0.0F) {
            return false;
        }
    }
    return true;
}

}  // namespace cy::rendering::fog
