// SPDX-License-Identifier: MIT
// AN AREA LIGHT OF THE FRAME, SHADED, AGAINST ITS OWN SURFACE.
//
// `shade_area_light` is what the forward pass adds for a rect, disc, sphere or tube light — the C++
// twin of `cy/light.slang`'s `shadeAreaLight`, which `render.area_lights` holds the device to. This
// file holds the twin to the light's physics: the direct term a path tracer would gather from the
// emitter, integrated over the emitter's TRUE surface — a disc's disc, a sphere's visible cap, a
// tube's cylinder — rather than over the quad the evaluator substitutes. The tolerances below are
// the statement of what each substitution costs.
//
// Integration rather than unit: the table is built once (`LtcTable::build`), and each reference is
// tens of thousands of BRDF evaluations.

#include <cy/test/test.h>

#include <cy/core/math/math.h>
#include <cy/core/math/quat.h>
#include <cy/rendering/lighting/area_shading.h>
#include <cy/rendering/lighting/lights.h>
#include <cy/rendering/lighting/units.h>

#include <cmath>
#include <cstdio>

namespace {

using cy::f32;
using cy::f64;
using cy::u32;
using cy::Vec3;
using cy::render::LightDescription;
using cy::render::LightKind;
using cy::rendering::AreaShading;
using cy::rendering::AreaSurface;
using cy::rendering::GpuLight;

constexpr f32 kPi = cy::math::kPi;

const cy::rendering::LtcTable& table() noexcept {
    static const cy::rendering::LtcTable built = [] {
        cy::rendering::LtcTable made;
        made.build();
        return made;
    }();
    return built;
}

GpuLight record_of(const LightDescription& light) noexcept {
    cy::rendering::LightBuildParameters parameters;
    parameters.unit = cy::rendering::default_unit_for(light.kind);
    return cy::rendering::build_gpu_light(light, parameters);
}

/// A light at `position` whose forward axis is `forward` (a rect's and a disc's emitting normal).
LightDescription light_at(LightKind kind, f32 width, f32 height, Vec3 position,
                          Vec3 forward) noexcept {
    LightDescription light;
    light.kind = kind;
    light.transform.translation = position;
    // A forward axis near the vertical takes the world's -Z as its up, so a panel facing straight
    // down has a defined width axis.
    const Vec3 unit = cy::normalize(forward);
    const Vec3 up = std::fabs(unit.y) > 0.9F ? Vec3{0.0F, 0.0F, -1.0F} : Vec3{0.0F, 1.0F, 0.0F};
    light.transform.rotation = cy::Quat::look_rotation(unit, up);
    light.source_width = width;
    light.source_height = height;
    light.intensity = 1000.0F;  // nits
    light.range = 1000.0F;      // the window is one everywhere these cases look
    return light;
}

// --- The GGX BRDF, as `cy/brdf.slang` and `cy/material.slang` shade a punctual light -------------

f32 ggx_d(f32 n_dot_h, f32 roughness) noexcept {
    const f32 alpha = roughness * roughness;
    const f32 a2 = alpha * alpha;
    const f32 denominator = (n_dot_h * n_dot_h * (a2 - 1.0F)) + 1.0F;
    return a2 / (kPi * denominator * denominator);
}

f32 smith_v(f32 n_dot_v, f32 n_dot_l, f32 roughness) noexcept {
    const f32 alpha = roughness * roughness;
    const f32 a2 = alpha * alpha;
    const f32 view = n_dot_l * std::sqrt((n_dot_v * n_dot_v * (1.0F - a2)) + a2);
    const f32 light = n_dot_v * std::sqrt((n_dot_l * n_dot_l * (1.0F - a2)) + a2);
    return 0.5F / cy::math::max(view + light, 1e-7F);
}

/// `specularGgx`'s value times the cosine, for F0 = 0.04: the specular radiance per unit incoming
/// radiance and solid angle.
f32 specular_cosine(Vec3 normal, Vec3 view, Vec3 light, f32 roughness) noexcept {
    const f32 n_dot_l = cy::math::max(cy::dot(normal, light), 0.0F);
    if (n_dot_l <= 0.0F) {
        return 0.0F;
    }
    const Vec3 half = cy::normalize(view + light);
    const f32 n_dot_v = cy::math::max(cy::dot(normal, view), 0.0F) + 1e-5F;
    const f32 v_dot_h = cy::math::max(cy::dot(view, half), 0.0F);
    const f32 fresnel = 0.04F + (0.96F * std::pow(1.0F - v_dot_h, 5.0F));
    return ggx_d(cy::math::max(cy::dot(normal, half), 0.0F), roughness) *
           smith_v(n_dot_v, n_dot_l, roughness) * fresnel * n_dot_l;
}

/// One emitter sample: a point on its surface, the surface's normal there and the area it stands
/// for.
struct SurfacePoint {
    Vec3 position;
    Vec3 normal;
    f32 area = 0.0F;
};

/// The emitter's TRUE surface, as a quadrature: `visit(point)` for each cell.
template <typename Visit>
void for_each_surface_point(const LightDescription& light, Visit&& visit) noexcept {
    constexpr u32 kSteps = 160;
    const Vec3 centre = light.transform.translation;
    const Vec3 forward = cy::normalize(light.transform.forward());
    const Vec3 right = cy::normalize(light.transform.right());
    const Vec3 up = cy::normalize(light.transform.up());
    const f32 width = light.source_width;
    const f32 height = light.source_height;
    for (u32 i = 0; i < kSteps; ++i) {
        const f32 a = (static_cast<f32>(i) + 0.5F) / static_cast<f32>(kSteps);
        for (u32 j = 0; j < kSteps; ++j) {
            const f32 b = (static_cast<f32>(j) + 0.5F) / static_cast<f32>(kSteps);
            const f32 cell = 1.0F / static_cast<f32>(kSteps * kSteps);
            SurfacePoint point;
            switch (light.kind) {
                case LightKind::Rect:
                    point.position =
                        centre + right * ((a - 0.5F) * width) + up * ((b - 0.5F) * height);
                    point.normal = forward;
                    point.area = width * height * cell;
                    break;
                case LightKind::Disc: {
                    // Polar cells, equal in area: radius by sqrt.
                    const f32 radius = 0.5F * width;
                    const f32 r = radius * std::sqrt(a);
                    const f32 angle = 2.0F * kPi * b;
                    point.position =
                        centre + right * (r * std::cos(angle)) + up * (r * std::sin(angle));
                    point.normal = forward;
                    point.area = kPi * radius * radius * cell;
                    break;
                }
                case LightKind::Sphere: {
                    // Equal-area cells: z uniform in [-1, 1].
                    const f32 radius = 0.5F * width;
                    const f32 z = (2.0F * a) - 1.0F;
                    const f32 ring = std::sqrt(cy::math::max(1.0F - (z * z), 0.0F));
                    const f32 angle = 2.0F * kPi * b;
                    point.normal = Vec3{ring * std::cos(angle), ring * std::sin(angle), z};
                    point.position = centre + point.normal * radius;
                    point.area = 4.0F * kPi * radius * radius * cell;
                    break;
                }
                case LightKind::Tube: {
                    const f32 radius = 0.5F * height;
                    const f32 angle = 2.0F * kPi * b;
                    point.normal = up * std::cos(angle) + forward * std::sin(angle);
                    point.position = centre + right * ((a - 0.5F) * width) + point.normal * radius;
                    point.area = 2.0F * kPi * radius * width * cell;
                    break;
                }
                default:
                    return;
            }
            visit(point);
        }
    }
}

/// The reference: the emitter's radiance gathered over its surface — irradiance for the diffuse
/// term, the GGX lobe for the specular.
struct Reference {
    f64 irradiance = 0.0;
    f64 specular = 0.0;
};

Reference gather(const LightDescription& light, Vec3 position, Vec3 normal, Vec3 view,
                 f32 roughness) noexcept {
    Reference reference;
    const f32 luminance = light.intensity;
    for_each_surface_point(light, [&](const SurfacePoint& point) noexcept {
        const Vec3 offset = point.position - position;
        const f32 distance_squared = cy::length_squared(offset);
        const Vec3 direction = offset * (1.0F / std::sqrt(distance_squared));
        // The emitter faces the receiver at this cell, or the cell does not emit towards it. A
        // two-sided panel emits from either face.
        f32 emitter_cosine = -cy::dot(point.normal, direction);
        if (light.two_sided) {
            emitter_cosine = std::fabs(emitter_cosine);
        }
        const f32 receiver_cosine = cy::dot(normal, direction);
        if (emitter_cosine <= 0.0F || receiver_cosine <= 0.0F) {
            return;
        }
        const f32 solid_angle = point.area * emitter_cosine / distance_squared;
        reference.irradiance += static_cast<f64>(luminance * receiver_cosine * solid_angle);
        reference.specular += static_cast<f64>(
            luminance * specular_cosine(normal, view, direction, roughness) * solid_angle);
    });
    return reference;
}

struct Configuration {
    const char* name = "";
    LightKind kind = LightKind::Rect;
    f32 width = 0.0F;
    f32 height = 0.0F;
    Vec3 position;
    Vec3 forward;
};

/// The relative error of the twin's diffuse term against the reference's, for an albedo of one:
/// the diffuse radiance is `E / pi`.
f32 diffuse_error(const Configuration& configuration, Vec3 receiver, Vec3 normal) noexcept {
    const LightDescription light =
        light_at(configuration.kind, configuration.width, configuration.height,
                 configuration.position, configuration.forward);
    AreaSurface surface;
    surface.albedo = Vec3{1.0F, 1.0F, 1.0F};
    surface.roughness = 1.0F;
    const AreaShading twin = cy::rendering::shade_area_light(
        table(), record_of(light), receiver, normal, Vec3{0.0F, 1.0F, 0.0F}, surface);
    const Reference reference = gather(light, receiver, normal, Vec3{0.0F, 1.0F, 0.0F}, 1.0F);
    const f64 expected = reference.irradiance / static_cast<f64>(kPi);
    const f64 measured = static_cast<f64>(twin.diffuse.x);
    std::printf("  %-28s diffuse %.5f against %.5f — %+.3f%%\n", configuration.name, measured,
                expected, 100.0 * ((measured / expected) - 1.0));
    return static_cast<f32>(std::fabs((measured / expected) - 1.0));
}

}  // namespace

CY_TEST_CASE("area lights: every shape's diffuse term is its own surface's, integrated") {
    // A floor point facing up, and each emitter above it at a distance a few times its size —
    // where an emitter lights a room — tilted so the cosines are not trivial.
    const Vec3 receiver{0.0F, 0.0F, 0.0F};
    const Vec3 normal{0.0F, 1.0F, 0.0F};
    const Configuration exact[] = {
        {"rect, overhead", LightKind::Rect, 1.0F, 0.5F, Vec3{0.2F, 2.0F, 0.1F},
         Vec3{0.0F, -1.0F, 0.0F}},
        {"rect, tilted and offset", LightKind::Rect, 0.8F, 0.8F, Vec3{1.0F, 1.5F, -0.5F},
         Vec3{-0.4F, -1.0F, 0.3F}},
        {"rect, close", LightKind::Rect, 2.0F, 1.0F, Vec3{0.0F, 0.6F, 0.0F},
         Vec3{0.0F, -1.0F, 0.0F}},
    };
    // The rect is not an approximation: the evaluator integrates its exact polygon.
    for (const Configuration& configuration : exact) {
        CY_CHECK_LT(diffuse_error(configuration, receiver, normal), 0.005F);
    }
    // The disc and the sphere are equal-area squares and the tube its silhouette: what each
    // substitution costs at the distances an emitter lights a room from, measured.
    const Configuration substituted[] = {
        {"disc, overhead", LightKind::Disc, 0.6F, 0.0F, Vec3{0.0F, 1.5F, 0.0F},
         Vec3{0.0F, -1.0F, 0.0F}},
        {"disc, tilted", LightKind::Disc, 0.6F, 0.0F, Vec3{0.8F, 1.2F, 0.0F},
         Vec3{-0.5F, -1.0F, 0.0F}},
        {"sphere, overhead", LightKind::Sphere, 0.4F, 0.0F, Vec3{0.0F, 1.5F, 0.0F},
         Vec3{0.0F, 0.0F, -1.0F}},
        {"sphere, low", LightKind::Sphere, 0.4F, 0.0F, Vec3{2.0F, 0.6F, 0.0F},
         Vec3{0.0F, 0.0F, -1.0F}},
        {"tube, overhead", LightKind::Tube, 1.2F, 0.06F, Vec3{0.0F, 1.5F, 0.0F},
         Vec3{0.0F, -1.0F, 0.0F}},
        {"tube, along and offset", LightKind::Tube, 1.2F, 0.06F, Vec3{0.6F, 1.0F, 0.8F},
         Vec3{0.0F, 0.0F, -1.0F}},
    };
    for (const Configuration& configuration : substituted) {
        CY_CHECK_LT(diffuse_error(configuration, receiver, normal), 0.03F);
    }
}

CY_TEST_CASE("area lights: the specular term is the GGX lobe's over the emitter, with Fresnel") {
    // A rect over a glossy floor, seen from several directions and roughnesses. The error is
    // reported against the set's PEAK, as test_reference.cpp reports the table's: a configuration
    // whose lobe misses the light has a reference of a few parts in a thousand, and a ratio there
    // says nothing about what a viewer sees.
    const Vec3 receiver{0.0F, 0.0F, 0.0F};
    const Vec3 normal{0.0F, 1.0F, 0.0F};
    const LightDescription panel =
        light_at(LightKind::Rect, 1.0F, 0.6F, Vec3{0.0F, 1.6F, -1.2F}, Vec3{0.0F, -1.0F, 0.4F});
    const GpuLight record = record_of(panel);
    struct Case {
        Vec3 view;
        f32 roughness = 0.0F;
    };
    const Case cases[] = {
        {Vec3{0.0F, 0.6F, 0.8F}, 0.3F},  {Vec3{0.0F, 0.6F, 0.8F}, 0.6F},
        {Vec3{0.0F, 0.6F, 0.8F}, 0.9F},  {Vec3{0.3F, 0.8F, 0.5F}, 0.45F},
        {Vec3{0.0F, 1.0F, 0.01F}, 0.5F}, {Vec3{-0.5F, 0.5F, 0.7F}, 0.7F},
    };
    f64 peak = 0.0;
    f64 errors[sizeof(cases) / sizeof(cases[0])] = {};
    u32 index = 0;
    for (const Case& configuration : cases) {
        const Vec3 view = cy::normalize(configuration.view);
        AreaSurface surface;
        surface.albedo = Vec3{0.0F, 0.0F, 0.0F};  // a dielectric with no diffuse: F0 = 0.04
        surface.roughness = configuration.roughness;
        const AreaShading twin =
            cy::rendering::shade_area_light(table(), record, receiver, normal, view, surface);
        const Reference reference = gather(panel, receiver, normal, view, configuration.roughness);
        std::printf("  roughness %.2f view (%.2f %.2f %.2f): specular %.4f against %.4f\n",
                    static_cast<f64>(configuration.roughness), static_cast<f64>(view.x),
                    static_cast<f64>(view.y), static_cast<f64>(view.z),
                    static_cast<f64>(twin.specular.x), reference.specular);
        peak = cy::math::max(peak, reference.specular);
        errors[index++] = std::fabs(static_cast<f64>(twin.specular.x) - reference.specular);
    }
    f64 mean = 0.0;
    f64 worst = 0.0;
    for (const f64 error : errors) {
        mean += error / peak;
        worst = cy::math::max(worst, error / peak);
    }
    mean /= static_cast<f64>(index);
    std::printf("  specular: mean absolute error %.3f%% of the set's peak; worst %.3f%%\n",
                100.0 * mean, 100.0 * worst);
    CY_CHECK_LT(mean, 0.05);
    CY_CHECK_LT(worst, 0.15);
}

CY_TEST_CASE("area lights: a lumen is a lumen whatever the shape, out in the far field") {
    // Every shape given the same flux in LUMENS lights a far receiver facing it as that flux says:
    // a one-sided panel `Phi / (pi d^2)` on its axis, a sphere `Phi / (4 pi d^2)`, a tube broadside
    // `Phi / (pi^2 d^2)`. The unit conversion and the shading agree about the emitting area.
    const f32 flux = 800.0F;
    const f32 distance = 25.0F;
    struct Case {
        LightKind kind;
        f32 width;
        f32 height;
        f32 expected;
    };
    const Case cases[] = {
        {LightKind::Rect, 0.6F, 0.4F, flux / (kPi * distance * distance)},
        {LightKind::Disc, 0.5F, 0.0F, flux / (kPi * distance * distance)},
        {LightKind::Sphere, 0.3F, 0.0F, flux / (4.0F * kPi * distance * distance)},
        {LightKind::Tube, 1.0F, 0.05F, flux / (kPi * kPi * distance * distance)},
    };
    for (const Case& configuration : cases) {
        LightDescription light =
            light_at(configuration.kind, configuration.width, configuration.height,
                     Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F});
        cy::rendering::LightBuildParameters parameters;
        parameters.unit = cy::rendering::PhotometricUnit::Lumen;
        light.intensity = flux;
        const GpuLight record = cy::rendering::build_gpu_light(light, parameters);
        // On the forward axis for the panels; broadside for the tube, whose axis is `right`.
        const Vec3 receiver{0.0F, 0.0F, -distance};
        const Vec3 irradiance =
            cy::rendering::light_irradiance(record, receiver, Vec3{0.0F, 0.0F, 1.0F});
        CY_CHECK_NEAR_REL(irradiance.x, configuration.expected, 0.01F);
    }
}
