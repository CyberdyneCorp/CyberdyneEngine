#include <cy/rendering/post/effects.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering {
namespace {

[[nodiscard]] f32 luminance_of(Vec3 colour) noexcept {
    return (0.2126F * colour.x) + (0.7152F * colour.y) + (0.0722F * colour.z);
}

}  // namespace

Vec3 apply_ambient_occlusion(Vec3 direct, Vec3 indirect, f32 visibility,
                             const AmbientOcclusionSettings& settings) noexcept {
    const f32 term = std::pow(math::saturate(visibility), math::max(settings.power, 1e-3F));
    const Vec3 occluded_indirect{indirect.x * term, indirect.y * term, indirect.z * term};
    if (!settings.apply_to_direct) {
        // The default, and the requirement: a directly lit surface is unaffected.
        return direct + occluded_indirect;
    }
    const f32 strength = math::saturate(settings.direct_strength);
    const f32 direct_term = math::lerp(1.0F, term, strength);
    return Vec3{(direct.x * direct_term) + occluded_indirect.x,
                (direct.y * direct_term) + occluded_indirect.y,
                (direct.z * direct_term) + occluded_indirect.z};
}

f32 specular_occlusion(Vec3 bent_normal, Vec3 reflection_direction, f32 visibility,
                       f32 roughness) noexcept {
    // The visibility cone's half angle widens as the surface is more occluded; the specular lobe's
    // widens with roughness. What survives is how much of the lobe falls inside the cone, and the
    // alignment between the bent normal and the reflection is what decides that.
    const f32 clamped_visibility = math::saturate(visibility);
    const f32 alignment =
        math::saturate(dot(normalize(bent_normal), normalize(reflection_direction)));
    const f32 lobe = math::saturate(roughness);
    // A rough lobe samples a wide range of directions and is forgiving of misalignment; a mirror is
    // not, which is why a smooth surface facing an occluded direction loses its reflection.
    const f32 tolerance = math::lerp(0.05F, 1.0F, lobe);
    const f32 inside = math::saturate(1.0F - ((1.0F - alignment) / math::max(tolerance, 1e-3F)));
    return math::saturate(
        math::lerp(clamped_visibility * clamped_visibility, 1.0F, inside * clamped_visibility));
}

f32 froxel_slice_depth(const FroxelVolume& volume, u32 index) noexcept {
    const u32 slices = math::max(volume.depth, 1U);
    const f32 t = static_cast<f32>(math::min(index + 1U, slices)) / static_cast<f32>(slices);
    const f32 shaped = std::pow(t, math::max(volume.depth_exponent, 1.0F));
    return math::lerp(volume.near_plane, volume.far_plane, shaped);
}

u32 froxel_slice_of(const FroxelVolume& volume, f32 view_depth) noexcept {
    const u32 slices = math::max(volume.depth, 1U);
    const f32 span = math::max(volume.far_plane - volume.near_plane, 1e-6F);
    const f32 normalised = math::saturate((view_depth - volume.near_plane) / span);
    const f32 exponent = math::max(volume.depth_exponent, 1.0F);
    const f32 t = std::pow(normalised, 1.0F / exponent);
    const f32 index = std::ceil(t * static_cast<f32>(slices)) - 1.0F;
    return static_cast<u32>(math::clamp(index, 0.0F, static_cast<f32>(slices - 1U)));
}

f32 henyey_greenstein(f32 g, f32 cos_theta) noexcept {
    const f32 clamped = math::clamp(g, -0.99F, 0.99F);
    const f32 squared = clamped * clamped;
    const f32 denominator = 1.0F + squared - (2.0F * clamped * cos_theta);
    return (1.0F - squared) /
           (4.0F * math::kPi * denominator * std::sqrt(math::max(denominator, 1e-6F)));
}

ScatteringStep integrate_froxel(Vec3 in_scattering, f32 density, f32 thickness, Vec3 accumulated,
                                f32 transmittance) noexcept {
    // The analytic integral over the slice rather than a point sample at its centre. The difference
    // is visible as banding across slice boundaries in exactly the froxel volumes that are coarse
    // enough to be affordable.
    const f32 extinction = math::max(density, 0.0F) * math::max(thickness, 0.0F);
    const f32 slice_transmittance = std::exp(-extinction);
    const f32 integrated =
        extinction > 1e-6F ? (1.0F - slice_transmittance) / math::max(density, 1e-6F) : thickness;
    ScatteringStep step;
    step.scattering = accumulated + Vec3{in_scattering.x * integrated * transmittance,
                                         in_scattering.y * integrated * transmittance,
                                         in_scattering.z * integrated * transmittance};
    step.transmittance = transmittance * slice_transmittance;
    return step;
}

f32 circle_of_confusion(const DepthOfFieldSettings& settings, f32 distance_metres) noexcept {
    const f32 focal_metres = math::max(settings.focal_length_mm, 1.0F) * 0.001F;
    const f32 focus = math::max(settings.focus_distance, focal_metres * 1.01F);
    const f32 distance = math::max(distance_metres, 1e-4F);
    const f32 aperture_diameter = focal_metres / math::max(settings.aperture, 0.5F);

    // The textbook thin-lens circle of confusion, in the same units as the sensor, then normalised
    // by the sensor height so the result is a fraction of the image and independent of format.
    const f32 magnitude = aperture_diameter * (std::fabs(distance - focus) / distance) *
                          (focal_metres / math::max(focus - focal_metres, 1e-5F));
    const f32 sensor_metres = math::max(settings.sensor_height_mm, 1.0F) * 0.001F;
    const f32 fraction = magnitude / sensor_metres * settings.artistic_scale;
    // Negative in front of the focus plane. The sign is what the near/far split reads.
    return distance < focus ? -fraction : fraction;
}

f32 track_focus(f32 current_distance, f32 target_distance, f32 delta_seconds,
                const DepthOfFieldSettings& settings) noexcept {
    if (delta_seconds <= 0.0F) {
        return current_distance;
    }
    const f32 blend = 1.0F - std::exp(-math::max(settings.autofocus_speed, 0.0F) * delta_seconds);
    return math::lerp(current_distance, target_distance, math::clamp(blend, 0.0F, 1.0F));
}

f32 motion_blur_length(f32 velocity_pixels, f32 shutter_degrees, f32 scale) noexcept {
    // A 360° shutter is open for the whole frame, so it smears a whole frame of motion; 180° smears
    // half of one. The requirement's scenario is this ratio and nothing else.
    const f32 open_fraction = math::clamp(shutter_degrees, 0.0F, 360.0F) / 360.0F;
    return velocity_pixels * open_fraction * math::max(scale, 0.0F);
}

f32 motion_blur_weight(f32 centre_depth, f32 sample_depth, f32 sample_velocity,
                       f32 distance_pixels) noexcept {
    // A sample only contributes if its own motion could have carried it here. Combined with the
    // depth comparison, that is what keeps a static background sharp behind a fast object: the
    // background's velocity is zero, so it reaches nowhere.
    const f32 reach = math::saturate((math::max(sample_velocity, 0.0F) - distance_pixels) + 1.0F);
    // Foreground samples blur over background ones and not the other way round.
    const f32 nearer = sample_depth < centre_depth ? 1.0F : 0.0F;
    const f32 relative =
        math::saturate(1.0F - (std::fabs(centre_depth - sample_depth) /
                               math::max(math::min(centre_depth, sample_depth), 1e-3F)));
    return reach * math::max(nearer, relative);
}

Vec3 bloom_prefilter(Vec3 colour, const BloomSettings& settings) noexcept {
    const f32 luminance = luminance_of(colour);
    const f32 threshold = math::max(settings.threshold, 0.0F);
    const f32 knee = math::max(settings.knee, 0.0F) * threshold;
    // The quadratic soft knee: zero below `threshold - knee`, the full excess above
    // `threshold + knee`, and a smooth ramp between. A hard cut produces a boundary that crawls
    // across a gradient as the camera moves, which is the artefact the knee exists for.
    f32 contribution = 0.0F;
    if (knee > 1e-6F) {
        const f32 soft = math::clamp(luminance - threshold + knee, 0.0F, 2.0F * knee);
        contribution = (soft * soft) / (4.0F * knee);
    }
    const f32 above = math::max(luminance - threshold, contribution);
    const f32 scale = luminance > 1e-6F ? above / luminance : 0.0F;
    return Vec3{colour.x * scale, colour.y * scale, colour.z * scale};
}

f32 karis_weight(f32 luminance) noexcept {
    return 1.0F / (1.0F + math::max(luminance, 0.0F));
}

Vec3 bloom_composite(Vec3 scene, Vec3 bloom, const BloomSettings& settings) noexcept {
    // Energy conserving by construction: the scene is scaled down by exactly the fraction the bloom
    // adds. "Total image energy SHALL be approximately preserved, redistributed rather than added."
    const f32 mix = math::saturate(settings.intensity);
    return Vec3{math::lerp(scene.x, bloom.x, mix), math::lerp(scene.y, bloom.y, mix),
                math::lerp(scene.z, bloom.z, mix)};
}

}  // namespace cy::rendering
