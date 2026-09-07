// Ambient occlusion, volumetric fog, depth of field, motion blur and bloom — the parameter
// derivations, each against the scenario its requirement states.

#include <cy/test/test.h>

#include <cy/rendering/post/effects.h>

#include <cmath>

namespace {

using cy::rendering::AmbientOcclusionSettings;
using cy::rendering::BloomSettings;
using cy::rendering::DepthOfFieldSettings;
using cy::rendering::FroxelVolume;
using cy::rendering::henyey_greenstein;
using cy::rendering::integrate_froxel;
using cy::rendering::karis_weight;
using cy::rendering::motion_blur_length;
using cy::rendering::motion_blur_weight;
using cy::rendering::specular_occlusion;

}  // namespace

CY_TEST_CASE("ambient occlusion darkens the ambient contribution and leaves direct light alone") {
    const cy::Vec3 direct{2.0F, 2.0F, 2.0F};
    const cy::Vec3 indirect{0.4F, 0.4F, 0.4F};
    AmbientOcclusionSettings settings;

    const cy::Vec3 open = apply_ambient_occlusion(direct, indirect, 1.0F, settings);
    const cy::Vec3 occluded = apply_ambient_occlusion(direct, indirect, 0.25F, settings);
    CY_CHECK_LT(occluded.x, open.x);
    // The direct term is untouched: the difference is exactly the indirect term's loss.
    CY_CHECK_NEAR(open.x - occluded.x, indirect.x * 0.75F, 1e-5F);
    CY_CHECK_NEAR(apply_ambient_occlusion(direct, cy::Vec3{0.0F, 0.0F, 0.0F}, 0.0F, settings).x,
                  direct.x, 1e-6F);

    // The non-physical artistic option, off by default and visible when it is on.
    settings.apply_to_direct = true;
    settings.direct_strength = 1.0F;
    CY_CHECK_LT(apply_ambient_occlusion(direct, indirect, 0.25F, settings).x, occluded.x);

    // The bent normal improves specular occlusion: a reflection pointing into the occluded
    // direction survives less than one pointing away from it.
    const cy::Vec3 bent{0.0F, 1.0F, 0.0F};
    const cy::f32 aligned = specular_occlusion(bent, cy::Vec3{0.0F, 1.0F, 0.0F}, 0.4F, 0.1F);
    const cy::f32 away = specular_occlusion(bent, cy::Vec3{0.0F, -1.0F, 0.2F}, 0.4F, 0.1F);
    CY_CHECK_GT(aligned, away);
    // A rough lobe is forgiving of the misalignment a mirror is not.
    CY_CHECK_GT(specular_occlusion(bent, cy::Vec3{0.7F, 0.7F, 0.0F}, 0.4F, 0.9F),
                specular_occlusion(bent, cy::Vec3{0.7F, 0.7F, 0.0F}, 0.4F, 0.05F));
}

CY_TEST_CASE(
    "the froxel depth distribution and its inverse agree, and slices thicken with distance") {
    FroxelVolume volume;
    volume.depth = 64;
    volume.near_plane = 0.1F;
    volume.far_plane = 64.0F;

    // A distribution and an inverse that disagree produce fog that swims as the camera moves.
    for (cy::u32 slice = 0; slice < volume.depth; ++slice) {
        const cy::f32 far_edge = froxel_slice_depth(volume, slice);
        const cy::f32 probe = far_edge - 1e-3F;
        CY_CHECK_EQ(froxel_slice_of(volume, probe), slice);
    }
    CY_CHECK_NEAR(froxel_slice_depth(volume, volume.depth - 1U), volume.far_plane, 1e-3F);
    CY_CHECK_EQ(froxel_slice_of(volume, -5.0F), 0U);
    CY_CHECK_EQ(froxel_slice_of(volume, 1e6F), volume.depth - 1U);

    // Exponential: the far slices are the thick ones, which is what keeps the near ones small where
    // a froxel's world size matters.
    const cy::f32 first = froxel_slice_depth(volume, 0) - volume.near_plane;
    const cy::f32 last = froxel_slice_depth(volume, volume.depth - 1U) -
                         froxel_slice_depth(volume, volume.depth - 2U);
    CY_CHECK_GT(last, first * 10.0F);
}

CY_TEST_CASE("forward scattering brightens fog toward a light, and the integral conserves energy") {
    // g above zero scatters forward: looking toward the light is brighter than looking away.
    CY_CHECK_GT(henyey_greenstein(0.7F, 1.0F), henyey_greenstein(0.7F, -1.0F));
    CY_CHECK_LT(henyey_greenstein(-0.7F, 1.0F), henyey_greenstein(-0.7F, -1.0F));
    // Isotropic is the uniform sphere, whatever the angle.
    CY_CHECK_NEAR(henyey_greenstein(0.0F, 1.0F), 1.0F / (4.0F * cy::math::kPi), 1e-6F);
    CY_CHECK_NEAR(henyey_greenstein(0.0F, -0.3F), 1.0F / (4.0F * cy::math::kPi), 1e-6F);

    // Transmittance falls monotonically and never below zero, whatever the slice thickness.
    cy::f32 transmittance = 1.0F;
    cy::Vec3 accumulated{0.0F, 0.0F, 0.0F};
    for (cy::u32 slice = 0; slice < 32; ++slice) {
        const cy::rendering::ScatteringStep step =
            integrate_froxel(cy::Vec3{1.0F, 1.0F, 1.0F}, 0.1F, 0.5F, accumulated, transmittance);
        CY_CHECK_LE(step.transmittance, transmittance);
        CY_CHECK_GE(step.transmittance, 0.0F);
        CY_CHECK_GE(step.scattering.x, accumulated.x);
        accumulated = step.scattering;
        transmittance = step.transmittance;
    }
    CY_CHECK_LT(transmittance, 0.3F);
    // Zero density leaves the frame exactly as it was.
    const cy::rendering::ScatteringStep clear =
        integrate_froxel(cy::Vec3{1.0F, 1.0F, 1.0F}, 0.0F, 1.0F, cy::Vec3{0.0F, 0.0F, 0.0F}, 1.0F);
    CY_CHECK_NEAR(clear.transmittance, 1.0F, 1e-6F);
}

CY_TEST_CASE("the circle of confusion is physical, signed, and tracks focus") {
    DepthOfFieldSettings settings;
    settings.focal_length_mm = 50.0F;
    settings.aperture = 2.8F;
    settings.focus_distance = 5.0F;

    // In focus is sharp.
    CY_CHECK_NEAR(circle_of_confusion(settings, 5.0F), 0.0F, 1e-5F);
    // NEGATIVE in front of the focus plane and positive behind it. The sign is what the near/far
    // split reads: near-field blur must bleed OVER in-focus geometry, and a magnitude-only circle
    // of confusion cannot express which side a sample is on.
    CY_CHECK_LT(circle_of_confusion(settings, 1.0F), 0.0F);
    CY_CHECK_GT(circle_of_confusion(settings, 40.0F), 0.0F);

    // Opening the aperture blurs more; a longer lens blurs more; both are physical.
    DepthOfFieldSettings wide = settings;
    wide.aperture = 1.4F;
    CY_CHECK_GT(std::fabs(circle_of_confusion(wide, 1.0F)),
                std::fabs(circle_of_confusion(settings, 1.0F)) * 1.9F);
    DepthOfFieldSettings tele = settings;
    tele.focal_length_mm = 135.0F;
    CY_CHECK_GT(std::fabs(circle_of_confusion(tele, 20.0F)),
                std::fabs(circle_of_confusion(settings, 20.0F)));

    // The artistic override scales the physical result rather than replacing it.
    DepthOfFieldSettings dialled = settings;
    dialled.artistic_scale = 2.0F;
    CY_CHECK_NEAR(circle_of_confusion(dialled, 20.0F), circle_of_confusion(settings, 20.0F) * 2.0F,
                  1e-6F);

    // Autofocus tracks at a configured speed rather than snapping.
    const cy::f32 stepped = track_focus(5.0F, 15.0F, 1.0F / 60.0F, settings);
    CY_CHECK_GT(stepped, 5.0F);
    CY_CHECK_LT(stepped, 15.0F);
    CY_CHECK_EQ(track_focus(5.0F, 15.0F, 0.0F, settings), 5.0F);
}

CY_TEST_CASE(
    "blur length follows the shutter, and the background stays sharp behind a fast object") {
    // At 180 degrees the blur is half a frame of motion. The requirement's scenario as an equation.
    CY_CHECK_NEAR(motion_blur_length(40.0F, 180.0F), 20.0F, 1e-5F);
    CY_CHECK_NEAR(motion_blur_length(40.0F, 360.0F), 40.0F, 1e-5F);
    CY_CHECK_NEAR(motion_blur_length(40.0F, 0.0F), 0.0F, 1e-6F);

    // A fast foreground object reaches a nearby pixel; a static background does not reach anywhere,
    // so it does not smear over the object.
    const cy::f32 foreground_reaches = motion_blur_weight(20.0F, 4.0F, 30.0F, 8.0F);
    const cy::f32 background_reaches = motion_blur_weight(4.0F, 20.0F, 0.0F, 8.0F);
    CY_CHECK_GT(foreground_reaches, 0.5F);
    CY_CHECK_NEAR(background_reaches, 0.0F, 1e-6F);
}

CY_TEST_CASE("bloom has a soft knee, suppresses fireflies, and redistributes rather than adds") {
    BloomSettings settings;
    settings.threshold = 1.0F;
    settings.knee = 0.5F;

    // Below the knee: nothing.
    CY_CHECK_NEAR(bloom_prefilter(cy::Vec3{0.3F, 0.3F, 0.3F}, settings).x, 0.0F, 1e-6F);
    // Inside the knee: a ramp, not a step. A hard cut produces a boundary that crawls across a
    // gradient as the camera moves.
    const cy::f32 inside = bloom_prefilter(cy::Vec3{0.9F, 0.9F, 0.9F}, settings).x;
    CY_CHECK_GT(inside, 0.0F);
    CY_CHECK_LT(inside, 0.1F);
    // Above it: the excess.
    CY_CHECK_GT(bloom_prefilter(cy::Vec3{4.0F, 4.0F, 4.0F}, settings).x, 2.5F);
    // Monotone through the knee.
    cy::f32 previous = -1.0F;
    for (cy::u32 step = 0; step < 40; ++step) {
        const cy::f32 value = static_cast<cy::f32>(step) * 0.1F;
        const cy::f32 filtered = bloom_prefilter(cy::Vec3{value, value, value}, settings).x;
        CY_CHECK_GE(filtered, previous - 1e-6F);
        previous = filtered;
    }

    // The Karis average: a very bright pixel is weighted down so it cannot produce a flickering
    // star. It is a weight rather than a clamp because a clamp also removes the energy.
    CY_CHECK_LT(karis_weight(400.0F), karis_weight(1.0F));
    CY_CHECK_NEAR(karis_weight(0.0F), 1.0F, 1e-6F);
    CY_CHECK_GT(karis_weight(400.0F), 0.0F);

    // Energy conserving: with bloom equal to the scene, the composite is the scene. Bloom
    // redistributes energy rather than adding it.
    const cy::Vec3 scene{2.0F, 1.0F, 0.5F};
    const cy::Vec3 same = bloom_composite(scene, scene, settings);
    CY_CHECK_NEAR(same.x, scene.x, 1e-6F);
    const cy::Vec3 mixed = bloom_composite(scene, cy::Vec3{0.0F, 0.0F, 0.0F}, settings);
    CY_CHECK_LT(mixed.x, scene.x);
    CY_CHECK_NEAR(mixed.x, scene.x * (1.0F - settings.intensity), 1e-5F);
}
