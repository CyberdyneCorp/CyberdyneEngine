// The four stages, and the behaviour every scenario in `denoising` names. Task 9.3.
//
// Integration rather than unit: an a-trous cascade over a 32x32 image costs more than the 1 ms of
// CPU the unit budget allows once the guidance weights are real, and shrinking the image below the
// filter's own reach would be measuring the border rather than the filter.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/denoise/denoiser.h>

#include "support.h"

#include <cmath>
#include <vector>

namespace {

using cy::rendering::denoise::Denoiser;
using cy::rendering::denoise::NoisySignal;
using cy::rendering::denoise::SignalConfig;
using cy::rendering::denoise::SignalKind;
using denoise_support::Scene;
using denoise_support::StillHistory;

/// The mean absolute error against the converged answer, over the interior of one half of the
/// image. The interior, because a border pixel's filter is clipped and that is a property of the
/// border rather than of the filter.
[[nodiscard]] cy::f32 mean_error(const Scene& scene, cy::Span<const cy::Vec3> image) {
    cy::f32 total = 0.0F;
    cy::f32 count = 0.0F;
    for (cy::u32 y = 4; y < scene.height - 4; ++y) {
        for (cy::u32 x = 4; x < scene.width - 4; ++x) {
            if (x >= (scene.width / 2) - 4 && x < (scene.width / 2) + 4) {
                continue;  // the boundary band belongs to the bleeding test
            }
            const cy::u32 pixel = (y * scene.width) + x;
            total += std::abs(image[pixel].x - scene.expected(x));
            count += 1.0F;
        }
    }
    return count > 0.0F ? total / count : 0.0F;
}

/// Run `frames` frames of the same still scene through one signal and return the last image.
std::vector<cy::Vec3> converge(Denoiser& denoiser, const Scene& scene, SignalKind kind,
                               cy::u32 frames, cy::f32 amplitude = 0.5F) {
    const cy::u32 pixels = scene.width * scene.height;
    const StillHistory history(pixels);
    std::vector<cy::Vec3> last;
    for (cy::u32 frame = 0; frame < frames; ++frame) {
        const std::vector<cy::Vec3> values = scene.sample(frame, amplitude);
        NoisySignal noisy;
        noisy.values = {values.data(), values.size()};
        const auto result = denoiser.denoise(kind, noisy, scene.guidance(), history.guidance());
        CY_REQUIRE(result.has_value());
        last.assign(result.value().begin(), result.value().end());
    }
    return last;
}

}  // namespace

CY_TEST_CASE("accumulation and filtering reconstruct a stable image from a noisy one") {
    const Scene scene;
    Denoiser denoiser;
    CY_REQUIRE(denoiser.resize(scene.width, scene.height).has_value());

    const std::vector<cy::Vec3> raw = scene.sample(0);
    const cy::f32 raw_error = mean_error(scene, {raw.data(), raw.size()});

    const std::vector<cy::Vec3> first = converge(denoiser, scene, SignalKind::IndirectDiffuse, 1);
    const std::vector<cy::Vec3> settled =
        converge(denoiser, scene, SignalKind::IndirectDiffuse, 24);

    // One frame of spatial filtering already helps; twenty-four frames of accumulation on top of it
    // is what makes the signal usable.
    CY_CHECK_LT(mean_error(scene, {first.data(), first.size()}), raw_error);
    CY_CHECK_LT(mean_error(scene, {settled.data(), settled.size()}),
                mean_error(scene, {first.data(), first.size()}));
    CY_CHECK_LT(mean_error(scene, {settled.data(), settled.size()}), raw_error * 0.2F);
}

CY_TEST_CASE("a converged signal is not blurred") {
    // `denoising`: "WHEN a region has accumulated many samples with low variance THEN spatial
    // filtering SHALL be reduced or skipped there."
    const Scene scene;

    // A noisy signal with two frames of history: high variance, few samples, filtered hard.
    Denoiser noisy;
    CY_REQUIRE(noisy.resize(scene.width, scene.height).has_value());
    (void)converge(noisy, scene, SignalKind::IndirectDiffuse, 2);
    const auto& early = noisy.diagnostics(SignalKind::IndirectDiffuse);
    CY_CHECK_GT(early.passes_applied, 0U);
    CY_CHECK_GT(early.mean_filter_radius, 0.0F);
    CY_CHECK_GT(early.mean_variance, 0.0F);

    // The same scene with no noise at all, accumulated past the converged sample count. Two
    // denoisers rather than one history carried between them, deliberately: the temporal filter's
    // whole job is to remember, so a run that had noise in it stays a little noisy for as many
    // frames as its declared history length, and a test that did not separate the two would be
    // measuring the history length instead of the convergence rule.
    Denoiser settled_denoiser;
    CY_REQUIRE(settled_denoiser.resize(scene.width, scene.height).has_value());
    (void)converge(settled_denoiser, scene, SignalKind::IndirectDiffuse, 16, 0.0F);
    const auto& settled = settled_denoiser.diagnostics(SignalKind::IndirectDiffuse);
    CY_CHECK_GE(settled.mean_sample_count, 12.0F);
    CY_CHECK_EQ(settled.mean_variance, 0.0F);
    CY_CHECK_EQ(settled.passes_applied, 0U);
    CY_CHECK_EQ(settled.mean_filter_radius, 0.0F);
}

CY_TEST_CASE("a disoccluded region widens the filter to compensate") {
    // `denoising`: "WHEN history is unavailable for a region THEN the spatial filter SHALL widen to
    // compensate for the missing temporal samples."
    const Scene scene;
    const cy::u32 pixels = scene.width * scene.height;
    Denoiser denoiser;
    CY_REQUIRE(denoiser.resize(scene.width, scene.height).has_value());

    (void)converge(denoiser, scene, SignalKind::IndirectDiffuse, 30, 0.0F);
    const cy::f32 converged_radius =
        denoiser.diagnostics(SignalKind::IndirectDiffuse).mean_filter_radius;

    // Now reject every pixel's history: the camera cut.
    StillHistory rejected(pixels, 0.0F);
    const std::vector<cy::Vec3> values = scene.sample(99);
    NoisySignal noisy;
    noisy.values = {values.data(), values.size()};
    CY_REQUIRE(
        denoiser.denoise(SignalKind::IndirectDiffuse, noisy, scene.guidance(), rejected.guidance())
            .has_value());

    const auto& after = denoiser.diagnostics(SignalKind::IndirectDiffuse);
    CY_CHECK_EQ(after.rejected_history_fraction, 1.0F);
    CY_CHECK_GT(after.passes_applied, 0U);
    CY_CHECK_GT(after.mean_filter_radius, converged_radius);
    CY_CHECK_LT(after.mean_sample_count, 2.0F);
}

CY_TEST_CASE("identity beats inference: no bleeding across an object edge") {
    // Two surfaces at the same depth with the same normals and very different values. Only the
    // visibility buffer's identifiers can separate them, which is exactly the scenario.
    const Scene scene(32, 0.05F, 0.95F);
    const cy::u32 pixels = scene.width * scene.height;
    Denoiser denoiser;
    CY_REQUIRE(denoiser.resize(scene.width, scene.height).has_value());
    (void)converge(denoiser, scene, SignalKind::IndirectDiffuse, 20, 0.3F);

    const std::vector<cy::Vec3> settled =
        converge(denoiser, scene, SignalKind::IndirectDiffuse, 4, 0.3F);

    // The two pixels either side of the boundary keep their own side's answer.
    const cy::u32 row = scene.height / 2;
    const cy::u32 left = (row * scene.width) + (scene.width / 2) - 1;
    const cy::u32 right = (row * scene.width) + (scene.width / 2);
    CY_CHECK_LT(std::abs(settled[left].x - scene.left_value), 0.06F);
    CY_CHECK_LT(std::abs(settled[right].x - scene.right_value), 0.06F);
    CY_CHECK(denoiser.diagnostics(SignalKind::IndirectDiffuse).identity_available);

    // The control: with the identifiers withheld, the same filter DOES bleed. Without this the
    // case above would pass on a filter that never crossed anything because it never filtered.
    Denoiser blind;
    CY_REQUIRE(blind.resize(scene.width, scene.height).has_value());
    auto guidance = scene.guidance();
    guidance.instance_id = {};
    guidance.material_id = {};
    // Depth and normals are identical either side, so nothing is left to stop on but the value.
    SignalConfig loose = cy::rendering::denoise::default_config(SignalKind::IndirectDiffuse);
    loose.sigma_value = 1000.0F;
    blind.configure(SignalKind::IndirectDiffuse, loose);

    const StillHistory history(pixels);
    std::vector<cy::Vec3> blind_image;
    for (cy::u32 frame = 0; frame < 24; ++frame) {
        const std::vector<cy::Vec3> values = scene.sample(frame, 0.3F);
        NoisySignal noisy;
        noisy.values = {values.data(), values.size()};
        const auto result =
            blind.denoise(SignalKind::IndirectDiffuse, noisy, guidance, history.guidance());
        CY_REQUIRE(result.has_value());
        blind_image.assign(result.value().begin(), result.value().end());
    }
    CY_CHECK_GT(std::abs(blind_image[right].x - scene.right_value), 0.06F);
    CY_CHECK_FALSE(blind.diagnostics(SignalKind::IndirectDiffuse).identity_available);
}

CY_TEST_CASE("a smooth reflection stays sharp and a rough one tolerates a wide filter") {
    // `denoising`: "WHEN a near-mirror reflection is denoised THEN the filter SHALL remain narrow,
    // so the reflection is not smeared." The width follows roughness through one declared field.
    Scene smooth(32);
    for (cy::f32& value : smooth.roughness) {
        value = 0.0F;
    }
    Scene rough(32);
    for (cy::f32& value : rough.roughness) {
        value = 1.0F;
    }

    Denoiser sharp_denoiser;
    CY_REQUIRE(sharp_denoiser.resize(smooth.width, smooth.height).has_value());
    (void)converge(sharp_denoiser, smooth, SignalKind::IndirectSpecular, 3);

    Denoiser wide_denoiser;
    CY_REQUIRE(wide_denoiser.resize(rough.width, rough.height).has_value());
    (void)converge(wide_denoiser, rough, SignalKind::IndirectSpecular, 3);

    CY_CHECK_GT(wide_denoiser.diagnostics(SignalKind::IndirectSpecular).mean_filter_radius,
                sharp_denoiser.diagnostics(SignalKind::IndirectSpecular).mean_filter_radius);

    // And a hemispherical signal is unaffected by roughness, because it declares zero widening.
    Denoiser diffuse_smooth;
    CY_REQUIRE(diffuse_smooth.resize(smooth.width, smooth.height).has_value());
    (void)converge(diffuse_smooth, smooth, SignalKind::IndirectDiffuse, 3);
    Denoiser diffuse_rough;
    CY_REQUIRE(diffuse_rough.resize(rough.width, rough.height).has_value());
    (void)converge(diffuse_rough, rough, SignalKind::IndirectDiffuse, 3);
    CY_CHECK_EQ(diffuse_rough.diagnostics(SignalKind::IndirectDiffuse).mean_filter_radius,
                diffuse_smooth.diagnostics(SignalKind::IndirectDiffuse).mean_filter_radius);
}

CY_TEST_CASE("shadow contact is preserved rather than filtered as colour") {
    // A hard shadow edge down the middle of the image, denoised as a visibility term and as a
    // radiance term with the same inputs. The visibility configuration keeps the edge; the
    // radiance one softens it. Both are the same filter, and the difference is one declared field.
    const Scene scene(32, 0.0F, 1.0F);
    const cy::u32 row = scene.height / 2;
    const cy::u32 left = (row * scene.width) + (scene.width / 2) - 1;
    const cy::u32 right = (row * scene.width) + (scene.width / 2);

    auto guidance = scene.guidance();
    // Withhold the identifiers: a shadow edge is not an object edge, so the filter has nothing to
    // stop on but the signal's own value — which is exactly the case the tolerance is for.
    guidance.instance_id = {};
    guidance.material_id = {};

    const StillHistory history(scene.width * scene.height);

    auto run = [&](SignalKind kind) {
        Denoiser denoiser;
        CY_REQUIRE(denoiser.resize(scene.width, scene.height).has_value());
        std::vector<cy::Vec3> image;
        for (cy::u32 frame = 0; frame < 8; ++frame) {
            const std::vector<cy::Vec3> values = scene.sample(frame, 0.15F);
            NoisySignal noisy;
            noisy.values = {values.data(), values.size()};
            const auto result = denoiser.denoise(kind, noisy, guidance, history.guidance());
            CY_REQUIRE(result.has_value());
            image.assign(result.value().begin(), result.value().end());
        }
        return image;
    };

    const std::vector<cy::Vec3> occlusion = run(SignalKind::RayTracedShadow);
    const std::vector<cy::Vec3> colour = run(SignalKind::StochasticDirect);

    const cy::f32 occlusion_step = occlusion[right].x - occlusion[left].x;
    const cy::f32 colour_step = colour[right].x - colour[left].x;
    CY_CHECK_GT(occlusion_step, colour_step);
    CY_CHECK_GT(occlusion_step, 0.5F);
}

CY_TEST_CASE("denoiser quality is a lever the budget can pull") {
    // `denoising`: "WHEN the GI allocation is reduced THEN denoiser passes or resolution SHALL be
    // reduced as one of the declared levers." The lever is observable in the diagnostics and in the
    // residual error, which is the trade-off the requirement asks to be reported.
    const Scene scene;
    Denoiser best;
    CY_REQUIRE(best.resize(scene.width, scene.height).has_value());
    best.set_quality_position(0);
    const std::vector<cy::Vec3> at_best = converge(best, scene, SignalKind::IndirectDiffuse, 3);
    const cy::u32 best_passes = best.diagnostics(SignalKind::IndirectDiffuse).passes_applied;
    const cy::u64 best_cost = best.diagnostics(SignalKind::IndirectDiffuse).cost_ns;

    Denoiser worst;
    CY_REQUIRE(worst.resize(scene.width, scene.height).has_value());
    worst.set_quality_position(3);
    const std::vector<cy::Vec3> at_worst = converge(worst, scene, SignalKind::IndirectDiffuse, 3);
    const cy::u32 worst_passes = worst.diagnostics(SignalKind::IndirectDiffuse).passes_applied;

    CY_CHECK_LT(worst_passes, best_passes);
    CY_CHECK_LT(worst.diagnostics(SignalKind::IndirectDiffuse).cost_ns, best_cost);
    // Coarser, not absent: the last rung still reconstructs something better than the raw signal.
    const std::vector<cy::Vec3> raw = scene.sample(2);
    CY_CHECK_LT(mean_error(scene, {at_worst.data(), at_worst.size()}),
                mean_error(scene, {raw.data(), raw.size()}));
    CY_CHECK_LT(mean_error(scene, {at_best.data(), at_best.size()}),
                mean_error(scene, {at_worst.data(), at_worst.size()}));

    // Out of range clamps to the coarsest rather than failing inside a frame.
    worst.set_quality_position(99);
    CY_CHECK_EQ(worst.quality_position(), 3U);
}
