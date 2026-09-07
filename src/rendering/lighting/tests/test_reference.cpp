// WHAT THE MOMENT-MATCHED LTC TABLE IS WORTH, measured against a Monte Carlo integration of the
// same GGX BRDF over the same rectangle.
//
// `area.h` is explicit that this implementation does NOT run the published L-BFGS fit — it uses the
// moment-matching initialisation the same paper starts from. A claim like that is worth nothing
// without a number beside it, and this file is the number. The tolerance below IS the statement of
// the approximation's quality; replacing the fit with the L-BFGS one is a change to
// `build_ltc_table` and to this tolerance and to nothing else.
//
// Integration rather than unit: building the table is 2.4 million BRDF evaluations and the
// reference is another 32,768 per configuration. Neither fits a 1 ms unit budget, and a reference
// shortened to fit one is not a reference — so every case that needs an `LtcTable` lives here,
// including the two that are about its SHAPE response rather than its accuracy.
//
// The reservoir estimator's bias measurement is here for the same reason and is the same kind of
// case: an expectation is only an expectation over enough samples to be one.

#include <cy/test/test.h>

#include <cy/core/math/math.h>
#include <cy/rendering/lighting/area.h>
#include <cy/rendering/lighting/many_light.h>

#include <cmath>

namespace {

using cy::f32;
using cy::f64;
using cy::u32;
using cy::Vec3;
using cy::rendering::AreaLight;
using cy::rendering::AreaQuad;
using cy::rendering::AreaLightShape;
using cy::rendering::LightCandidate;
using cy::rendering::ltc_evaluate;
using cy::rendering::ManyLightSettings;
using cy::rendering::Reservoir;
using cy::rendering::sample_lights;
using cy::rendering::SampleStream;
using cy::rendering::ltc_evaluate_diffuse;
using cy::rendering::LtcTable;

f32 ggx_d(f32 n_dot_h, f32 alpha) noexcept {
    const f32 a2 = alpha * alpha;
    const f32 denominator = n_dot_h * n_dot_h * (a2 - 1.0F) + 1.0F;
    return a2 / (cy::math::kPi * denominator * denominator);
}

f32 smith_v(f32 n_dot_l, f32 n_dot_v, f32 alpha) noexcept {
    const f32 a2 = alpha * alpha;
    const f32 lambda_v = n_dot_l * std::sqrt(n_dot_v * n_dot_v * (1.0F - a2) + a2);
    const f32 lambda_l = n_dot_v * std::sqrt(n_dot_l * n_dot_l * (1.0F - a2) + a2);
    const f32 sum = lambda_v + lambda_l;
    return sum > 0.0F ? 0.5F / sum : 0.0F;
}

/// The reference integrals are taken over the emitter's SURFACE, not over the hemisphere.
///
/// That is not a refinement, it is the difference between a reference and a rumour. A uniform
/// hemisphere sampler has to hit a light that subtends a fraction of a steradian, so a 181 x 181
/// grid lands a few dozen samples on a small panel at four metres and the "reference" carries a
/// 35% quantisation error of its own — measured, on the first draft of this file, against a closed
/// form that turned out to be right. Sampling the emitter and converting to solid angle with
/// `cos(theta_l) / r^2` puts every sample where the integrand is non-zero.
constexpr u32 kAreaSteps = 129;  // odd, so no sample lands exactly on the emitter's centre

/// Integrate `f(direction)` over the emitter's area, converted to solid angle.
template <typename F>
f32 integrate_over_emitter(const AreaLight& light, F&& f) noexcept {
    const Vec3 emitter_normal = normalize(cross(light.tangent_x, light.tangent_y));
    const f32 cell_area = (2.0F * light.half_x / static_cast<f32>(kAreaSteps)) *
                          (2.0F * light.half_y / static_cast<f32>(kAreaSteps));
    f32 total = 0.0F;
    for (u32 i = 0; i < kAreaSteps; ++i) {
        const f32 u = ((static_cast<f32>(i) + 0.5F) / static_cast<f32>(kAreaSteps)) * 2.0F - 1.0F;
        for (u32 j = 0; j < kAreaSteps; ++j) {
            const f32 v = ((static_cast<f32>(j) + 0.5F) / static_cast<f32>(kAreaSteps)) * 2.0F -
                          1.0F;
            const Vec3 point = light.center + light.tangent_x * (u * light.half_x) +
                               light.tangent_y * (v * light.half_y);
            const f32 distance_squared = length_squared(point);
            if (distance_squared <= 1.0e-8F) {
                continue;
            }
            const Vec3 direction = point / std::sqrt(distance_squared);
            const f32 cos_at_emitter = std::fabs(dot(direction, emitter_normal));
            total += f(direction) * (cell_area * cos_at_emitter / distance_squared);
        }
    }
    return total;
}

/// The GGX specular integral over the emitter.
f32 reference_specular(const AreaLight& light, Vec3 normal, Vec3 view, f32 roughness) noexcept {
    const f32 alpha = cy::math::max(roughness * roughness, 1.0e-4F);
    const f32 n_dot_v = cy::math::clamp(dot(normal, view), 1.0e-3F, 1.0F);
    return integrate_over_emitter(light, [&](Vec3 direction) {
        const f32 n_dot_l = dot(normal, direction);
        if (n_dot_l <= 0.0F) {
            return 0.0F;
        }
        const Vec3 half = normalize(direction + view);
        const f32 n_dot_h = cy::math::max(0.0F, dot(normal, half));
        return ggx_d(n_dot_h, alpha) * smith_v(n_dot_l, n_dot_v, alpha) * n_dot_l;
    });
}

/// The clamped-cosine integral over the emitter, normalised by pi so the answer is the same
/// quantity `ltc_evaluate_diffuse` gives. The diffuse LTC is the IDENTITY transform, so this case
/// has no fit in it and no excuse: a disagreement here is a defect in `integrate_cosine_polygon`.
f32 reference_diffuse(const AreaLight& light, Vec3 normal) noexcept {
    return integrate_over_emitter(light, [&](Vec3 direction) {
               const f32 n_dot_l = dot(normal, direction);
               return n_dot_l > 0.0F ? n_dot_l : 0.0F;
           }) /
           cy::math::kPi;
}


/// `emitter`'s other spelling: the two moved cases were written against a helper of this name and
/// keeping both makes the diff that moved them readable.
AreaLight overhead(f32 half, f32 distance) noexcept;

AreaLight emitter(f32 half, f32 distance) noexcept {
    AreaLight light;
    light.shape = AreaLightShape::Rect;
    light.center = Vec3{0.0F, 0.0F, distance};
    light.half_x = half;
    light.half_y = half;
    light.tangent_x = Vec3{1.0F, 0.0F, 0.0F};
    light.tangent_y = Vec3{0.0F, 1.0F, 0.0F};
    light.single_sided = false;
    return light;
}

AreaLight overhead(f32 half, f32 distance) noexcept {
    return emitter(half, distance);
}

}  // namespace

CY_TEST_CASE("area reference: the diffuse term matches the numeric integral to within 2%") {
    // The diffuse LTC is the identity transform, so this is the closed-form polygon integral
    // against a numeric one. It has no fit in it and therefore no excuse: a disagreement here is a
    // defect in `integrate_cosine_polygon`, not an approximation.
    const Vec3 normal{0.0F, 0.0F, 1.0F};
    f32 worst = 0.0F;
    for (f32 half : {0.3F, 1.0F, 3.0F}) {
        for (f32 distance : {1.5F, 4.0F}) {
            const AreaLight light = emitter(half, distance);
            const f32 measured = ltc_evaluate_diffuse(light, normal);
            const f32 reference = reference_diffuse(light, normal);
            const f32 error = std::fabs(measured - reference) /
                              cy::math::max(reference, 1.0e-4F);
            worst = cy::math::max(worst, error);
        }
    }
    CY_TEST_MESSAGE("diffuse: worst relative error over 6 configurations ", worst * 100.0F, "%");
    CY_CHECK_LT(worst, 0.02F);
}

CY_TEST_CASE("area reference: the moment-matched specular table's error is measured, not assumed") {
    LtcTable table;
    table.build();
    CY_REQUIRE(table.built());

    const Vec3 normal{0.0F, 0.0F, 1.0F};
    f32 worst = 0.0F;
    f32 sum = 0.0F;
    u32 count = 0;

    for (f32 roughness : {0.20F, 0.40F, 0.70F, 0.95F}) {
        for (f32 view_z : {0.98F, 0.60F}) {
            const Vec3 view =
                normalize(Vec3{std::sqrt(cy::math::max(0.0F, 1.0F - view_z * view_z)), 0.0F,
                               view_z});
            const AreaLight light = emitter(1.2F, 3.0F);
            const f32 measured = ltc_evaluate(table, light, normal, view, roughness);
            const f32 reference = reference_specular(light, normal, view, roughness);
            const f32 error = std::fabs(measured - reference) /
                              cy::math::max(reference, 1.0e-5F);
            worst = cy::math::max(worst, error);
            sum += error;
            ++count;
        }
    }

    const f32 mean = sum / static_cast<f32>(count);
    // The MEAN is the headline, not the worst: a single worst case over eight configurations is a
    // draw from a small sample, and M7's own artefact rule ("an artefact SHALL headline a stable
    // statistic, never an extreme") applies to a test's message for the same reason.
    CY_TEST_MESSAGE("specular: mean relative error ", mean * 100.0F, "%, worst ", worst * 100.0F,
                    "% over ", count, " configurations");

    // THIS TOLERANCE IS THE STATEMENT OF WHAT THE MOMENT-MATCHED TABLE IS WORTH. It is not tight
    // and it is not meant to be: the published L-BFGS fit reaches a few per cent, and this is the
    // initialisation that fit starts from. What the number buys is a highlight of the right SHAPE
    // and roughly the right energy with no stochastic sampling, which is the requirement; what it
    // does not buy is a match a reference renderer would accept.
    CY_CHECK_LT(mean, 0.45F);
    CY_CHECK_LT(worst, 0.80F);
}

CY_TEST_CASE("area reference: the table's energy falls with roughness rather than wandering") {
    // The property that matters more than the absolute error, because it is what a viewer sees: a
    // surface must not get brighter as it gets rougher. A fit can be several per cent off
    // everywhere and still look right; one that is non-monotone here shows as a band.
    LtcTable table;
    table.build();
    const Vec3 normal{0.0F, 0.0F, 1.0F};
    const Vec3 view = normalize(Vec3{0.3F, 0.0F, 0.95F});
    const AreaLight light = emitter(1.0F, 3.0F);

    f32 previous = ltc_evaluate(table, light, normal, view, 0.10F);
    for (u32 step = 1; step <= 20; ++step) {
        const f32 roughness = 0.10F + 0.85F * static_cast<f32>(step) / 20.0F;
        const f32 value = ltc_evaluate(table, light, normal, view, roughness);
        CY_CHECK_LE(value, previous * 1.05F);
        previous = value;
    }
}

CY_TEST_CASE("area: the highlight takes the light's shape and elongates with roughness") {
    // The requirement's own scenario: "WHEN a rectangular light illuminates a glossy surface THEN
    // the specular highlight SHALL take the light's shape, elongating with roughness."
    LtcTable table;
    table.build();
    CY_REQUIRE(table.built());

    AreaLight wide = overhead(2.0F, 3.0F);
    wide.half_y = 0.1F;  // a long thin strip
    AreaLight tall = overhead(0.1F, 3.0F);
    tall.half_y = 2.0F;

    const Vec3 normal{0.0F, 0.0F, 1.0F};
    const Vec3 view = normalize(Vec3{0.9F, 0.0F, 0.6F});

    // The two lights have identical area and identical distance, and differ only in which way the
    // strip runs. A point-light approximation cannot tell them apart; the LTC must.
    const f32 along = ltc_evaluate(table, wide, normal, view, 0.25F);
    const f32 across = ltc_evaluate(table, tall, normal, view, 0.25F);
    CY_CHECK_NE(along, across);

    // And the highlight broadens with roughness: a mirror sees a thin sliver of the strip, a rough
    // surface sees the whole of it, so the ratio between the two orientations narrows.
    const f32 rough_along = ltc_evaluate(table, wide, normal, view, 0.85F);
    const f32 rough_across = ltc_evaluate(table, tall, normal, view, 0.85F);
    const f32 sharp_ratio = along / cy::math::max(across, 1.0e-6F);
    const f32 rough_ratio = rough_along / cy::math::max(rough_across, 1.0e-6F);
    CY_CHECK_LT(std::fabs(rough_ratio - 1.0F), std::fabs(sharp_ratio - 1.0F));
}

CY_TEST_CASE("area: the LTC table is smooth in both parameters") {
    // The reason 32 x 32 is enough, stated as a property rather than as a claim: adjacent samples
    // differ by little, so the bilinear interpolation between them cannot be hiding a step.
    LtcTable table;
    table.build();
    AreaLight light = overhead(1.0F, 2.0F);
    const Vec3 normal{0.0F, 0.0F, 1.0F};
    const Vec3 view = normalize(Vec3{0.5F, 0.0F, 0.86F});

    f32 previous = ltc_evaluate(table, light, normal, view, 0.02F);
    for (u32 step = 1; step <= 48; ++step) {
        const f32 roughness = 0.02F + 0.96F * static_cast<f32>(step) / 48.0F;
        const f32 value = ltc_evaluate(table, light, normal, view, roughness);
        CY_CHECK_LT(std::fabs(value - previous), 0.35F);
        previous = value;
    }
}

CY_TEST_CASE("many-light: the reservoir estimator is unbiased against a sum over every light") {
    // The case that matters. `contribution_weight()` is `weight_sum / (M * target)`, and getting
    // that expression wrong looks like a scene that is a few per cent too bright in exactly the
    // places with the most lights — which is where nobody looks for an estimator bug.
    constexpr u32 kLights = 64;
    LightCandidate candidates[kLights];
    f32 reference = 0.0F;
    for (u32 index = 0; index < kLights; ++index) {
        candidates[index].index = index;
        // A deliberately skewed distribution: a handful of bright lights among many dim ones is
        // where uniform sampling does worst and where a wrong weight shows up first.
        candidates[index].unshadowed = (index % 16U == 0U) ? 40.0F : 0.3F;
        reference += candidates[index].unshadowed;
    }

    ManyLightSettings settings;
    settings.enabled = true;
    settings.denoising_available = true;
    settings.candidates_per_pixel = 16;

    // Average the estimator over many pixels, which is what a denoiser does spatially and what
    // makes the expectation the quantity that matters.
    f64 estimate = 0.0;
    constexpr u32 kPixels = 20000;
    for (u32 pixel = 0; pixel < kPixels; ++pixel) {
        SampleStream stream(pixel % 200U, pixel / 200U, 7U);
        const Reservoir reservoir =
            sample_lights(cy::Span<const LightCandidate>(candidates, kLights), settings, stream);
        if (!reservoir.valid()) {
            continue;
        }
        // Visibility is 1 here, so the estimate is the selected light's unshadowed contribution
        // times the reservoir's weight — and its expectation must be the sum over every light.
        estimate += static_cast<f64>(reservoir.target * reservoir.contribution_weight());
    }
    estimate /= static_cast<f64>(kPixels);

    const f64 relative = (estimate - static_cast<f64>(reference)) / static_cast<f64>(reference);
    CY_TEST_MESSAGE("reservoir estimate ", estimate, " against a reference sum of ", reference,
                    " — relative bias ", relative * 100.0, "%");
    CY_CHECK_LT(relative < 0.0 ? -relative : relative, 0.02);
}
