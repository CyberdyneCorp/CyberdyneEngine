// Distance attenuation, cones, panning, Doppler and filter-based occlusion. Task 4.3.5.
//
// `audio` — "Spatial audio". Every function under test is pure, which is what lets the panning law
// and the attenuation curves be asserted as numbers rather than listened to. The fallback path
// asserted here is the one `audio` requires to work with no acoustics backend present: "**WHEN**
// the engine is built without Steam Audio **THEN** all audio SHALL still play, spatialised by the
// fallback path, with no missing sounds and no gameplay difference."

#include <cy/core/math/scalar.h>
#include <cy/servers/audio/spatial.h>
#include <cy/test/test.h>

#include <cmath>

using cy::f32;
using namespace cy::audio;

CY_TEST_CASE(
    "inside the reference distance a source is at full gain, and past the maximum silent") {
    Attenuation attenuation;
    attenuation.reference_distance = 1.0F;
    attenuation.max_distance = 100.0F;

    for (const AttenuationModel model : {AttenuationModel::Inverse, AttenuationModel::InverseSquare,
                                         AttenuationModel::Linear, AttenuationModel::Logarithmic}) {
        attenuation.model = model;
        CY_CHECK_NEAR(attenuation_gain(attenuation, 0.0F), 1.0F, 1e-5F);
        CY_CHECK_NEAR(attenuation_gain(attenuation, 1.0F), 1.0F, 1e-5F);
        // SILENT AT THE MAXIMUM, IN EVERY MODEL. That is what makes a voice inaudible, and
        // inaudible is what makes it virtualisable rather than mixed forever.
        CY_CHECK_NEAR(attenuation_gain(attenuation, 100.0F), 0.0F, 1e-6F);
        CY_CHECK_NEAR(attenuation_gain(attenuation, 1000.0F), 0.0F, 1e-6F);
    }
}

CY_TEST_CASE("the inverse-square law halves amplitude every doubling of distance") {
    Attenuation attenuation;
    attenuation.model = AttenuationModel::InverseSquare;
    attenuation.reference_distance = 1.0F;
    attenuation.max_distance = 1000.0F;
    CY_CHECK_NEAR(attenuation_gain(attenuation, 2.0F), 0.25F, 1e-5F);
    CY_CHECK_NEAR(attenuation_gain(attenuation, 4.0F), 0.0625F, 1e-5F);
}

CY_TEST_CASE("the linear model reaches zero exactly at the maximum") {
    Attenuation attenuation;
    attenuation.model = AttenuationModel::Linear;
    attenuation.reference_distance = 10.0F;
    attenuation.max_distance = 20.0F;
    CY_CHECK_NEAR(attenuation_gain(attenuation, 15.0F), 0.5F, 1e-5F);
}

CY_TEST_CASE("every model is monotonically non-increasing with distance") {
    // A curve that rose anywhere would make a source get louder as the player walked away, which is
    // the kind of thing a single-point assertion misses.
    Attenuation attenuation;
    attenuation.reference_distance = 1.0F;
    attenuation.max_distance = 50.0F;
    for (const AttenuationModel model : {AttenuationModel::Inverse, AttenuationModel::InverseSquare,
                                         AttenuationModel::Linear, AttenuationModel::Logarithmic}) {
        attenuation.model = model;
        f32 previous = 2.0F;
        // Stepped by an integer and scaled, rather than accumulating a float: an accumulated loop
        // counter drifts, and clang-tidy is right that it should not be one.
        for (int step = 0; step < 120; ++step) {
            const f32 gain = attenuation_gain(attenuation, static_cast<f32>(step) * 0.5F);
            CY_CHECK_LE(gain, previous + 1e-5F);
            previous = gain;
        }
    }
}

namespace {

f32 half_gain(f32 normalised, void* user) noexcept {
    (void)user;
    return 1.0F - (normalised * 0.5F);
}

}  // namespace

CY_TEST_CASE("a project's own curve is sampled, and a missing one is loud rather than silent") {
    Attenuation attenuation;
    attenuation.model = AttenuationModel::Custom;
    attenuation.reference_distance = 0.0F;
    attenuation.max_distance = 100.0F;
    attenuation.curve = &half_gain;
    CY_CHECK_NEAR(attenuation_gain(attenuation, 50.0F), 0.75F, 1e-3F);

    // A `Custom` model with no curve is a configuration mistake. Full gain: a sound that is too
    // loud is noticed and fixed, and one that is silent is assumed missing.
    attenuation.curve = nullptr;
    CY_CHECK_NEAR(attenuation_gain(attenuation, 50.0F), 1.0F, 1e-5F);
}

CY_TEST_CASE("a default cone is omnidirectional") {
    // A zeroed struct must be a source that sounds the same in every direction, not one that is
    // silent everywhere.
    const Cone cone;
    CY_CHECK_NEAR(cone_gain(cone, cy::kAxisForward, cy::kAxisForward), 1.0F, 1e-5F);
    CY_CHECK_NEAR(cone_gain(cone, cy::kAxisForward, -cy::kAxisForward), 1.0F, 1e-5F);
}

CY_TEST_CASE("a cone is full gain inside, outer gain outside, and interpolated between") {
    Cone cone;
    cone.inner_angle_radians = cy::math::kPi * 0.5F;  // 90 degrees
    cone.outer_angle_radians = cy::math::kPi;         // 180 degrees
    cone.outer_gain = 0.2F;

    // Straight ahead: inside the inner cone.
    CY_CHECK_NEAR(cone_gain(cone, cy::kAxisForward, cy::kAxisForward), 1.0F, 1e-5F);
    // Straight behind: well outside the outer cone.
    CY_CHECK_NEAR(cone_gain(cone, cy::kAxisForward, -cy::kAxisForward), 0.2F, 1e-5F);
    // At the rim of the inner cone, still full gain.
    const cy::Vec3 rim = cy::normalize(cy::Vec3{0.70710678F, 0.0F, -0.70710678F});
    CY_CHECK_NEAR(cone_gain(cone, cy::kAxisForward, rim), 1.0F, 1e-4F);
    // And between the two rims it is somewhere in between.
    const cy::Vec3 between = cy::normalize(cy::Vec3{1.0F, 0.0F, -0.2F});
    const f32 gain = cone_gain(cone, cy::kAxisForward, between);
    CY_CHECK_GT(gain, 0.2F);
    CY_CHECK_LT(gain, 1.0F);
}

CY_TEST_CASE("stereo panning is constant power, so a source crossing the centre has no hole") {
    // A linear pan drops three decibels in the middle, which is audible as a hole. Constant power
    // keeps left² + right² at one everywhere, which is the assertion.
    for (int step = -10; step <= 10; ++step) {
        const f32 x = static_cast<f32>(step) * 0.1F;
        const PanGains gains = pan_stereo(cy::Vec3{x, 0.0F, -1.0F});
        const f32 power = (gains.left * gains.left) + (gains.right * gains.right);
        CY_CHECK_NEAR(power, 1.0F, 1e-4F);
    }
}

CY_TEST_CASE("a source on the right is louder on the right") {
    const PanGains right = pan_stereo(cy::kAxisRight);
    CY_CHECK_GT(right.right, 0.99F);
    CY_CHECK_LT(right.left, 0.01F);

    const PanGains left = pan_stereo(-cy::kAxisRight);
    CY_CHECK_GT(left.left, 0.99F);
    CY_CHECK_LT(left.right, 0.01F);

    const PanGains centre = pan_stereo(cy::kAxisForward);
    CY_CHECK_NEAR(centre.left, centre.right, 1e-5F);
}

CY_TEST_CASE("a source at the listener's own position is centred rather than a NaN") {
    const PanGains gains = pan_stereo(cy::Vec3{0.0F, 0.0F, 0.0F});
    CY_CHECK_NEAR(gains.left, gains.right, 1e-6F);
    CY_CHECK_NEAR((gains.left * gains.left) + (gains.right * gains.right), 1.0F, 1e-4F);
}

CY_TEST_CASE("front and back produce the same amplitude pan, which is the honest limit") {
    // Amplitude panning cannot distinguish front from back; HRTF at M8 is what does. Stating the
    // limit as an assertion is better than a front-back cue invented from nothing.
    const PanGains front = pan_stereo(cy::Vec3{0.3F, 0.0F, -1.0F});
    const PanGains back = pan_stereo(cy::Vec3{0.3F, 0.0F, 1.0F});
    CY_CHECK_NEAR(front.left, back.left, 1e-5F);
    CY_CHECK_NEAR(front.right, back.right, 1e-5F);
}

CY_TEST_CASE("a source approaching the listener rises in pitch") {
    // `audio`: "**WHEN** a source moves toward the listener at speed **THEN** its pitch SHALL rise
    // proportionally, scaled by the Doppler factor."
    const cy::Vec3 listener{0.0F, 0.0F, 0.0F};
    const cy::Vec3 source{0.0F, 0.0F, -100.0F};
    const cy::Vec3 approaching{0.0F, 0.0F, 30.0F};  // toward the listener

    const f32 rising = doppler_pitch(listener, cy::Vec3{}, source, approaching, 343.0F, 1.0F);
    CY_CHECK_GT(rising, 1.0F);

    const f32 receding = doppler_pitch(listener, cy::Vec3{}, source, -approaching, 343.0F, 1.0F);
    CY_CHECK_LT(receding, 1.0F);

    // A factor of zero disables it, which is what a project that does not want Doppler sets.
    CY_CHECK_NEAR(doppler_pitch(listener, cy::Vec3{}, source, approaching, 343.0F, 0.0F), 1.0F,
                  1e-6F);
    // A stationary pair is unshifted.
    CY_CHECK_NEAR(doppler_pitch(listener, cy::Vec3{}, source, cy::Vec3{}, 343.0F, 1.0F), 1.0F,
                  1e-5F);
}

CY_TEST_CASE("a supersonic source produces a high pitch rather than a negative one") {
    // Without the clamp the denominator crosses zero and the clip plays backwards at an enormous
    // rate — which is a crash-adjacent bug that only appears in the one scene with a jet in it.
    const f32 pitch = doppler_pitch(cy::Vec3{}, cy::Vec3{}, cy::Vec3{0.0F, 0.0F, -10.0F},
                                    cy::Vec3{0.0F, 0.0F, 400.0F}, 343.0F, 1.0F);
    CY_CHECK_GT(pitch, 1.0F);
    CY_CHECK_LE(pitch, 4.0F);
}

CY_TEST_CASE("occlusion both filters and attenuates") {
    // `audio`: "the source SHALL be low-pass filtered **and** attenuated by the occlusion
    // parameters." Returning only the cutoff is how the second half gets forgotten.
    f32 open_gain = 0.0F;
    const f32 open_cutoff = occlusion_filter(0.0F, 48000.0F, open_gain);
    CY_CHECK_NEAR(open_gain, 1.0F, 1e-5F);
    CY_CHECK_GT(open_cutoff, 20000.0F);

    f32 blocked_gain = 0.0F;
    const f32 blocked_cutoff = occlusion_filter(1.0F, 48000.0F, blocked_gain);
    CY_CHECK_LT(blocked_gain, 1.0F);
    CY_CHECK_GT(blocked_gain, 0.0F);  // muffled, not missing
    CY_CHECK_NEAR(blocked_cutoff, 400.0F, 1.0F);

    // And it is monotone in between.
    f32 previous_cutoff = open_cutoff;
    f32 previous_gain = open_gain;
    for (int step = 0; step <= 10; ++step) {
        f32 gain = 0.0F;
        const f32 cutoff = occlusion_filter(static_cast<f32>(step) * 0.1F, 48000.0F, gain);
        CY_CHECK_LE(cutoff, previous_cutoff + 1.0F);
        CY_CHECK_LE(gain, previous_gain + 1e-5F);
        previous_cutoff = cutoff;
        previous_gain = gain;
    }
}
