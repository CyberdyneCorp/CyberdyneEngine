// Colour grading, and the bake that has to agree with it. A bake that drifts from its reference is
// a grade that looks different in the editor and in the build.

#include <cy/test/test.h>

#include <cy/rendering/post/grading.h>
#include <cy/rendering/post/volumes.h>

#include <cmath>

namespace {

using cy::rendering::apply_grading;
using cy::rendering::blend_parameter;
using cy::rendering::GradingSettings;
using cy::rendering::PostParameterGroup;
using cy::rendering::PostVolume;
using cy::rendering::VolumeSample;

GradingSettings warm_and_contrasty() noexcept {
    GradingSettings settings;
    settings.temperature = 4200.0F;
    settings.tint = 0.1F;
    settings.contrast = 1.2F;
    settings.saturation = 0.85F;
    settings.gain = cy::Vec3{1.05F, 1.0F, 0.95F};
    settings.lift = cy::Vec3{0.01F, 0.0F, 0.02F};
    settings.shadows.gain = cy::Vec3{0.95F, 1.0F, 1.1F};
    settings.highlights.gain = cy::Vec3{1.05F, 1.0F, 0.95F};
    return settings;
}

}  // namespace

CY_TEST_CASE("the log encoding is invertible and spends its resolution on the shadows") {
    for (const cy::f32 linear : {0.001F, 0.01F, 0.18F, 1.0F, 8.0F, 100.0F}) {
        CY_CHECK_NEAR(cy::rendering::log_decode(cy::rendering::log_encode(linear)), linear,
                      linear * 1e-3F);
    }
    // The point of the encoding: a linear index would put almost nothing below 0.05, which is where
    // skin shadows and night interiors live. Here a quarter of the table is below middle grey.
    const cy::f32 grey = cy::rendering::log_encode(0.18F);
    CY_CHECK_GT(grey, 0.35F);
    CY_CHECK_LT(grey, 0.75F);
    // Monotone, and clamped at both ends rather than wrapping.
    CY_CHECK_LT(cy::rendering::log_encode(0.01F), cy::rendering::log_encode(0.02F));
    CY_CHECK_NEAR(cy::rendering::log_encode(1e-30F), 0.0F, 1e-6F);
    CY_CHECK_NEAR(cy::rendering::log_encode(1e30F), 1.0F, 1e-6F);
}

CY_TEST_CASE("a neutral grade changes nothing, and says so") {
    const GradingSettings neutral;
    CY_CHECK(neutral.neutral());
    CY_CHECK_FALSE(warm_and_contrasty().neutral());

    for (const cy::f32 value : {0.05F, 0.18F, 0.6F, 3.0F}) {
        const cy::Vec3 colour{value, value * 0.8F, value * 1.2F};
        const cy::Vec3 graded = apply_grading(colour, neutral);
        CY_CHECK_NEAR(graded.x, colour.x, cy::math::max(colour.x * 2e-2F, 1e-3F));
        CY_CHECK_NEAR(graded.y, colour.y, cy::math::max(colour.y * 2e-2F, 1e-3F));
        CY_CHECK_NEAR(graded.z, colour.z, cy::math::max(colour.z * 2e-2F, 1e-3F));
    }
}

CY_TEST_CASE("each control moves the image in the direction its name promises") {
    const cy::Vec3 grey{0.18F, 0.18F, 0.18F};
    const cy::Vec3 colourful{0.4F, 0.2F, 0.1F};

    GradingSettings warm;
    warm.temperature = 3000.0F;
    const cy::Vec3 warmed = apply_grading(grey, warm);
    CY_CHECK_GT(warmed.x, warmed.z);

    GradingSettings cool;
    cool.temperature = 9000.0F;
    const cy::Vec3 cooled = apply_grading(grey, cool);
    CY_CHECK_LT(cooled.x, cooled.z);

    GradingSettings desaturated;
    desaturated.saturation = 0.0F;
    const cy::Vec3 flat = apply_grading(colourful, desaturated);
    CY_CHECK_NEAR(flat.x, flat.y, 1e-4F);
    CY_CHECK_NEAR(flat.y, flat.z, 1e-4F);

    GradingSettings contrast;
    contrast.contrast = 2.0F;
    // Contrast is applied about middle grey, so raising it does not also raise exposure.
    CY_CHECK_NEAR(apply_grading(grey, contrast).y, 0.18F, 1e-3F);
    CY_CHECK_GT(apply_grading(cy::Vec3{0.6F, 0.6F, 0.6F}, contrast).y, 0.6F);

    GradingSettings mixer;
    mixer.channel_mixer[0] = cy::Vec3{0.0F, 1.0F, 0.0F};
    mixer.channel_mixer[1] = cy::Vec3{1.0F, 0.0F, 0.0F};
    const cy::Vec3 swapped = apply_grading(colourful, mixer);
    CY_CHECK_GT(swapped.y, swapped.x);

    // A hue rotation on a grey is a no-operation rather than a NaN, which is why the rotation is a
    // Rodrigues form and not an HSV round trip.
    GradingSettings hue;
    hue.hue_shift = 1.0F;
    const cy::Vec3 rotated_grey = apply_grading(grey, hue);
    CY_CHECK(std::isfinite(rotated_grey.x));
    CY_CHECK_NEAR(rotated_grey.x, 0.18F, 1e-3F);
    CY_CHECK_NE(apply_grading(colourful, hue).x, apply_grading(colourful, GradingSettings{}).x);
}

CY_TEST_CASE("entering a volume interpolates, and the higher priority dominates the overlap") {
    // The global volume, always a contributor, and an interior one the camera walks into.
    PostVolume global;
    global.unbounded = true;
    global.priority = 0;
    global.sets[static_cast<cy::usize>(PostParameterGroup::Grading)] = true;

    PostVolume interior;
    interior.priority = 10;
    interior.blend_distance = 2.0F;
    interior.sets[static_cast<cy::usize>(PostParameterGroup::Grading)] = true;

    const PostVolume volumes[] = {global, interior};
    cy::f32 weights[2] = {};

    // Well outside: only the global volume contributes.
    VolumeSample samples[2] = {VolumeSample{0.0F}, VolumeSample{5.0F}};
    CY_REQUIRE_EQ(blend_volume_weights(cy::Span<const PostVolume>(volumes, 2),
                                       cy::Span<const VolumeSample>(samples, 2),
                                       PostParameterGroup::Grading, weights, 2),
                  1U);
    CY_CHECK_NEAR(weights[0], 1.0F, 1e-6F);
    CY_CHECK_NEAR(weights[1], 0.0F, 1e-6F);

    // Halfway through the blend band: the interior volume dominates because it has the higher
    // priority, and the global one drops out of the blend entirely rather than being averaged with.
    samples[1] = VolumeSample{1.0F};
    CY_REQUIRE_EQ(blend_volume_weights(cy::Span<const PostVolume>(volumes, 2),
                                       cy::Span<const VolumeSample>(samples, 2),
                                       PostParameterGroup::Grading, weights, 2),
                  1U);
    CY_CHECK_NEAR(weights[1], 1.0F, 1e-6F);

    // Two volumes at the same priority share, normalised, so the result does not depend on how many
    // happen to overlap.
    PostVolume twin = interior;
    const PostVolume pair[] = {interior, twin};
    VolumeSample both[2] = {VolumeSample{0.0F}, VolumeSample{1.0F}};
    CY_REQUIRE_EQ(blend_volume_weights(cy::Span<const PostVolume>(pair, 2),
                                       cy::Span<const VolumeSample>(both, 2),
                                       PostParameterGroup::Grading, weights, 2),
                  2U);
    CY_CHECK_NEAR(weights[0] + weights[1], 1.0F, 1e-6F);
    CY_CHECK_GT(weights[0], weights[1]);

    // A volume that does not set a group contributes nothing to it — the per-parameter blend, which
    // is what stops an exposure volume dragging the grade toward its defaults.
    CY_CHECK_EQ(blend_volume_weights(cy::Span<const PostVolume>(volumes, 2),
                                     cy::Span<const VolumeSample>(samples, 2),
                                     PostParameterGroup::DepthOfField, weights, 2),
                0U);

    const cy::f32 values[2] = {2.0F, 8.0F};
    const cy::f32 even[2] = {0.5F, 0.5F};
    CY_CHECK_NEAR(
        blend_parameter(cy::Span<const cy::f32>(values, 2), cy::Span<const cy::f32>(even, 2), 0.0F),
        5.0F, 1e-6F);
    const cy::f32 none[2] = {0.0F, 0.0F};
    CY_CHECK_NEAR(
        blend_parameter(cy::Span<const cy::f32>(values, 2), cy::Span<const cy::f32>(none, 2), 3.0F),
        3.0F, 1e-6F);
}
