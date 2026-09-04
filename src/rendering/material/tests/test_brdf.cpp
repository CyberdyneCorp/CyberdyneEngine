// The core BRDF. Task 4.2.3.
//
// `rendering-materials-and-shading` states the BRDF concretely — "exact terms and their sources —
// because 'PBR' alone is not a specification and mismatched terms produce subtly wrong lighting
// that is very hard to debug later". These cases are the executable half of that statement: each
// one checks a PROPERTY of a term (its normalisation, its limit, its monotonicity) rather than a
// magic number, because a magic number recorded from the implementation would agree with a wrong
// implementation.
//
// The three scenarios the specification names are the three that matter most, and they are the last
// three cases here: metal has no diffuse, a rough metal conserves energy, and a dark dielectric
// does not get an unphysically bright rim at grazing angles.

#include <cy/core/math/scalar.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/material/brdf.h>
#include <cy/test/test.h>

#include <cmath>

using cy::f32;
using cy::u32;
using cy::Vec3;
using namespace cy::rendering;

namespace {

constexpr f32 kPi = cy::math::kPi;

/// A head-on sample: normal, view and light all coincident, so every cosine is one. The
/// configuration a hand calculation can check.
[[nodiscard]] LightSample head_on() noexcept {
    LightSample light;
    light.n_dot_l = 1.0F;
    light.n_dot_v = 1.0F;
    light.n_dot_h = 1.0F;
    light.v_dot_h = 1.0F;
    light.l_dot_v = 1.0F;
    return light;
}

[[nodiscard]] f32 luminance(Vec3 value) noexcept {
    return (0.2126F * value.x) + (0.7152F * value.y) + (0.0722F * value.z);
}

}  // namespace

CY_TEST_CASE("roughness is perceptual, and alpha is its square with a floor under it") {
    // "roughness SHALL be perceptual in material authoring (α = roughness²), and SHALL be clamped
    // to a minimum (default 0.045) to keep highlights representable."
    CY_CHECK_NEAR(alpha_from_roughness(0.5F), 0.25F, 1e-6F);
    CY_CHECK_NEAR(alpha_from_roughness(1.0F), 1.0F, 1e-6F);
    // Below the floor, and at zero, the clamp is what stops D from becoming a delta the framebuffer
    // cannot represent — the highlight that flickers between frames at a sub-pixel scale.
    CY_CHECK_NEAR(alpha_from_roughness(0.0F), kMinPerceptualRoughness * kMinPerceptualRoughness,
                  1e-9F);
    CY_CHECK_NEAR(alpha_from_roughness(-1.0F), kMinPerceptualRoughness * kMinPerceptualRoughness,
                  1e-9F);
}

CY_TEST_CASE("GGX peaks at the half-vector and falls away from it") {
    const f32 alpha = 0.25F;
    const f32 peak = distribution_ggx(1.0F, alpha);
    CY_CHECK_GT(peak, 0.0F);
    // Strictly decreasing away from NoH = 1, which is the whole shape of a specular lobe.
    f32 previous = peak;
    for (u32 step = 1; step <= 10; ++step) {
        const f32 n_dot_h = 1.0F - (static_cast<f32>(step) * 0.1F);
        const f32 value = distribution_ggx(n_dot_h, alpha);
        CY_CHECK_LT(value, previous);
        previous = value;
    }
    // At α = 1 the distribution is uniform over the hemisphere: D = 1/π everywhere. The one value
    // in the whole term that can be checked in closed form, and it pins the normalisation.
    CY_CHECK_NEAR(distribution_ggx(1.0F, 1.0F), 1.0F / kPi, 1e-5F);
    CY_CHECK_NEAR(distribution_ggx(0.3F, 1.0F), 1.0F / kPi, 1e-5F);
}

CY_TEST_CASE("a smoother surface concentrates the lobe rather than brightening the surface") {
    // The property that makes roughness readable as a material parameter: the peak rises as the
    // lobe narrows. A term that got this backwards would still look plausible in a screenshot.
    CY_CHECK_GT(distribution_ggx(1.0F, 0.05F), distribution_ggx(1.0F, 0.5F));
    CY_CHECK_GT(distribution_ggx(1.0F, 0.5F), distribution_ggx(1.0F, 1.0F));
}

CY_TEST_CASE("Smith visibility is finite at grazing angles and positive everywhere") {
    for (u32 step = 0; step <= 10; ++step) {
        const f32 cosine = 0.05F + (static_cast<f32>(step) * 0.09F);
        const f32 v = visibility_smith_hammon(cosine, cosine, 0.5F);
        CY_CHECK_GT(v, 0.0F);
        CY_CHECK(std::isfinite(v));
    }
    // Both cosines zero is the degenerate case, and it is answered rather than dividing.
    CY_CHECK_EQ(visibility_smith_hammon(0.0F, 0.0F, 0.0F), 0.0F);
}

CY_TEST_CASE("Fresnel is f0 head-on and f90 at grazing") {
    const Vec3 f0{0.04F, 0.04F, 0.04F};
    const Vec3 straight = fresnel_schlick(f0, 1.0F, 1.0F);
    CY_CHECK_NEAR(straight.x, 0.04F, 1e-6F);
    const Vec3 grazing = fresnel_schlick(f0, 1.0F, 0.0F);
    CY_CHECK_NEAR(grazing.x, 1.0F, 1e-6F);
    // And monotone between them: a non-monotone Fresnel produces a ring rather than a rim.
    f32 previous = 0.04F;
    for (u32 step = 10; step > 0; --step) {
        const f32 value = fresnel_schlick(f0, 1.0F, static_cast<f32>(step) * 0.1F).x;
        CY_CHECK_GE(value, previous - 1e-6F);
        previous = value;
    }
}

CY_TEST_CASE("f0 is the dielectric constant for an insulator and the albedo for a metal") {
    const Vec3 albedo{0.9F, 0.6F, 0.2F};
    const Vec3 dielectric = compute_f0(albedo, 0.0F, 1.0F);
    CY_CHECK_NEAR(dielectric.x, kDielectricF0, 1e-6F);
    CY_CHECK_NEAR(dielectric.z, kDielectricF0, 1e-6F);

    const Vec3 metal = compute_f0(albedo, 1.0F, 1.0F);
    CY_CHECK_NEAR(metal.x, albedo.x, 1e-6F);
    CY_CHECK_NEAR(metal.y, albedo.y, 1e-6F);

    // The specular parameter scales the dielectric f0 quadratically, so 0 is a surface with no
    // specular response at all rather than one that is merely dimmer.
    CY_CHECK_NEAR(compute_f0(albedo, 0.0F, 0.0F).x, 0.0F, 1e-6F);
    CY_CHECK_LT(compute_f0(albedo, 0.0F, 0.5F).x, kDielectricF0);
}

CY_TEST_CASE("f90 occludes the rim of a very dark f0 and is one for an ordinary surface") {
    // "f90 = saturate(dot(f0, vec3(50/3))), providing specular occlusion for very dark f0." The
    // scenario: "WHEN a very dark dielectric is viewed at a grazing angle THEN the f90 term SHALL
    // prevent an unphysically bright rim."
    CY_CHECK_NEAR(compute_f90(Vec3{0.04F, 0.04F, 0.04F}), 1.0F, 1e-6F);
    const f32 dark = compute_f90(Vec3{0.005F, 0.005F, 0.005F});
    CY_CHECK_LT(dark, 1.0F);
    CY_CHECK_GT(dark, 0.0F);
    CY_CHECK_EQ(compute_f90(Vec3{0.0F, 0.0F, 0.0F}), 0.0F);

    // And the consequence, which is what the requirement is actually about: the grazing response of
    // a near-black dielectric stays below that of an ordinary one.
    const Vec3 dark_f0{0.005F, 0.005F, 0.005F};
    const Vec3 ordinary_f0{0.04F, 0.04F, 0.04F};
    const f32 dark_rim = fresnel_schlick(dark_f0, compute_f90(dark_f0), 0.0F).x;
    const f32 ordinary_rim = fresnel_schlick(ordinary_f0, compute_f90(ordinary_f0), 0.0F).x;
    CY_CHECK_LT(dark_rim, ordinary_rim);
}

CY_TEST_CASE("every diffuse model integrates to about albedo over the hemisphere") {
    // Lambert is 1/π exactly; the other two are within a few per cent of it, which is the property
    // that says none of them is off by a factor of π — the single most common way to get a diffuse
    // term wrong, and one that looks like "the lights are too bright" rather than like a bug.
    CY_CHECK_NEAR(diffuse_lambert(), 1.0F / kPi, 1e-6F);
    CY_CHECK_NEAR(diffuse_burley(1.0F, 1.0F, 1.0F, 0.0F), 1.0F / kPi, 1e-5F);
    CY_CHECK_NEAR(diffuse_oren_nayar(1.0F, 1.0F, 1.0F, 0.0F), 1.0F / kPi, 1e-5F);

    for (u32 step = 0; step <= 10; ++step) {
        const f32 roughness = static_cast<f32>(step) * 0.1F;
        const f32 burley = diffuse_burley(0.7F, 0.7F, 0.5F, roughness);
        const f32 oren = diffuse_oren_nayar(0.7F, 0.7F, 0.5F, roughness);
        CY_CHECK_GT(burley, 0.0F);
        CY_CHECK_GT(oren, 0.0F);
        CY_CHECK_LT(burley, 2.0F / kPi);
        CY_CHECK_LT(oren, 2.0F / kPi);
    }
    // Every model has a name, which is what a diagnostic prints.
    for (u32 index = 0; index < static_cast<u32>(DiffuseModel::Count); ++index) {
        CY_CHECK(diffuse_model_name(static_cast<DiffuseModel>(index))[0] != '\0');
    }
}

CY_TEST_CASE("a metal has no diffuse") {
    // The specification's scenario, verbatim: "WHEN metallic is 1.0 THEN the diffuse term SHALL be
    // zero and f0 SHALL be the albedo."
    SurfaceParameters metal;
    metal.albedo = Vec3{0.9F, 0.9F, 0.9F};
    metal.metallic = 1.0F;
    metal.roughness = 0.4F;
    metal.energy_compensation = false;

    SurfaceParameters dielectric = metal;
    dielectric.metallic = 0.0F;

    const LightSample light = head_on();
    // With the specular half subtracted, what is left of the metal is nothing.
    const Vec3 metal_total = evaluate_direct(metal, light);
    const Vec3 metal_specular = evaluate_specular(metal, light);
    CY_CHECK_NEAR(metal_total.x - metal_specular.x, 0.0F, 1e-6F);

    const Vec3 dielectric_total = evaluate_direct(dielectric, light);
    const Vec3 dielectric_specular = evaluate_specular(dielectric, light);
    CY_CHECK_GT(dielectric_total.x - dielectric_specular.x, 0.1F);
}

CY_TEST_CASE("multi-scatter compensation returns the energy single-scatter GGX loses") {
    // "WHEN a metal with roughness 0.9 is lit THEN multi-scatter compensation SHALL keep its total
    // reflectance close to f0, rather than darkening as single-scatter GGX would."
    SurfaceParameters rough_metal;
    rough_metal.albedo = Vec3{1.0F, 1.0F, 1.0F};
    rough_metal.metallic = 1.0F;
    rough_metal.roughness = 0.9F;

    const LightSample light = head_on();
    rough_metal.energy_compensation = false;
    const Vec3 uncompensated = evaluate_specular(rough_metal, light);
    rough_metal.energy_compensation = true;
    const Vec3 compensated = evaluate_specular(rough_metal, light);

    CY_CHECK_GT(luminance(compensated), luminance(uncompensated));
    // The compensation is a gain, never a loss, and it is unity for a black f0 — a surface that
    // reflects nothing has no multiple scattering to recover.
    const Vec3 unity = multiscatter_compensation(Vec3{0.0F, 0.0F, 0.0F}, 0.9F, 0.05F);
    CY_CHECK_NEAR(unity.x, 1.0F, 1e-6F);
    const Vec3 gain = multiscatter_compensation(Vec3{1.0F, 1.0F, 1.0F}, 0.8F, 0.05F);
    CY_CHECK_GT(gain.x, 1.0F);
    // And the rougher the surface, the more there is to give back.
    const Vec3 rougher = multiscatter_compensation(Vec3{1.0F, 1.0F, 1.0F}, 0.6F, 0.05F);
    CY_CHECK_GT(rougher.x, gain.x);
}

CY_TEST_CASE("a light below the horizon contributes nothing") {
    SurfaceParameters surface;
    LightSample light = head_on();
    light.n_dot_l = -0.5F;
    const Vec3 result = evaluate_direct(surface, light);
    CY_CHECK_EQ(result.x, 0.0F);
    CY_CHECK_EQ(result.y, 0.0F);
    CY_CHECK_EQ(result.z, 0.0F);
}

CY_TEST_CASE("indirect specular is the split sum, and roughness selects a mip") {
    // "WHEN indirect specular is evaluated for roughness 0.5 THEN the corresponding pre-filtered
    // mip SHALL be sampled and scaled by the DFG table's (scale, bias) for the material's f0."
    const Vec3 radiance{2.0F, 2.0F, 2.0F};
    const Vec3 f0{0.04F, 0.04F, 0.04F};
    const DfgEntry dfg{0.9F, 0.05F};
    const Vec3 result = indirect_specular(radiance, f0, 1.0F, dfg);
    CY_CHECK_NEAR(result.x, 2.0F * ((0.04F * 0.9F) + (1.0F * 0.05F)), 1e-5F);

    // Linear in perceptual roughness, from the sharpest mip to the roughest.
    CY_CHECK_NEAR(prefiltered_mip_for(0.0F, 6), 0.0F, 1e-6F);
    CY_CHECK_NEAR(prefiltered_mip_for(1.0F, 6), 5.0F, 1e-6F);
    CY_CHECK_NEAR(prefiltered_mip_for(0.5F, 6), 2.5F, 1e-6F);
    // A single-mip environment has nowhere to go, and says so rather than indexing mip −1.
    CY_CHECK_NEAR(prefiltered_mip_for(0.7F, 1), 0.0F, 1e-6F);
}

CY_TEST_CASE("irradiance is never negative, however the coefficients were fitted") {
    // A ringing L2 fit can go negative, and a negative irradiance subtracts light from a surface —
    // which shows up as a black patch nobody can trace back to the environment probe.
    ShL2 sh;
    sh.coefficients[0] = Vec3{0.1F, 0.1F, 0.1F};
    sh.coefficients[2] = Vec3{-5.0F, -5.0F, -5.0F};
    const Vec3 value = sh.irradiance(Vec3{0.0F, 0.0F, 1.0F});
    CY_CHECK_GE(value.x, 0.0F);
    CY_CHECK_GE(value.y, 0.0F);
    CY_CHECK_GE(value.z, 0.0F);
}
