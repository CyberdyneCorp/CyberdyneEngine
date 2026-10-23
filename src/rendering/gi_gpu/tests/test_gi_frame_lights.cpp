// SPDX-License-Identifier: MIT
// unit.render_gi_frame_lights — the frame's area lights as the GI's emitters.
//
// A rect, disc, sphere or tube light reaches CyberGI as the emitter squares that cover its surface
// (`append_gi_lights`), and every consumer of `gi::shaded_direct` — the surface cache, the device
// card shading, the path tracer's next event, the irradiance volume's capture — integrates those
// squares in closed form. What is asserted: the squares radiate the light's flux, and a receiver a
// few emitter sizes away is lit by them as the frame's own area shading lights it, in the GI's
// units (irradiance over pi).

#include <cy/core/math/math.h>
#include <cy/core/math/quat.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi/lighting.h>
#include <cy/rendering/gi_gpu/frame_lights.h>
#include <cy/rendering/lighting/area_shading.h>
#include <cy/rendering/lighting/lights.h>
#include <cy/rendering/lighting/units.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstdio>

using namespace cy;
namespace gi = cy::rendering::gi;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

render::LightDescription area(render::LightKind kind, f32 width, f32 height) noexcept {
    render::LightDescription light;
    light.kind = kind;
    light.transform.translation = Vec3{0.0F, 3.0F, 0.0F};
    // Facing straight down; the forward axis's up is world -Z.
    light.transform.rotation =
        Quat::look_rotation(Vec3{0.0F, -1.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F});
    light.source_width = width;
    light.source_height = height;
    light.intensity = 2000.0F;  // nits
    light.range = 50.0F;
    light.stable_id = 31;
    return light;
}

/// The frame's irradiance at a receiver, over pi: the GI's `direct` for the same light.
Vec3 frame_direct(const render::LightDescription& light, Vec3 receiver, Vec3 normal) noexcept {
    rendering::LightBuildParameters parameters;
    parameters.unit = rendering::default_unit_for(light.kind);
    const rendering::GpuLight record = rendering::build_gpu_light(light, parameters);
    return rendering::light_irradiance(record, receiver, normal) * (1.0F / math::kPi);
}

}  // namespace

CY_TEST_CASE("gi frame lights: an area light's squares radiate its flux") {
    const render::LightKind kinds[4] = {render::LightKind::Rect, render::LightKind::Disc,
                                        render::LightKind::Sphere, render::LightKind::Tube};
    for (const render::LightKind kind : kinds) {
        for (const bool two_sided : {false, true}) {
            render::LightDescription light =
                area(kind, 1.2F, kind == render::LightKind::Tube ? 0.06F : 0.4F);
            light.two_sided = two_sided;
            Array<gi::GiLight> lights(allocator());
            const u32 appended = rendering::gi_gpu::append_gi_lights(light, lights);
            CY_REQUIRE(appended > 0U);
            if (kind == render::LightKind::Sphere) {
                // A point of L r^2 radiates 4 pi L r^2 in the GI's units: pi L times the sphere's
                // area, the sphere's flux.
                CY_REQUIRE_EQ(appended, 1U);
                CY_CHECK_EQ(lights[0].emitter_area, 0.0F);
                CY_CHECK_NEAR_REL(4.0F * math::kPi * lights[0].intensity,
                                  light.intensity * rendering::area_light_emitting_area(light),
                                  1e-4F);
                CY_CHECK_EQ(lights[0].radius, 0.6F);
                continue;
            }
            f32 area_sum = 0.0F;
            for (const gi::GiLight& square : lights) {
                CY_CHECK_GT(square.emitter_area, 0.0F);
                CY_CHECK_EQ(square.intensity, light.intensity);
                CY_CHECK_EQ(square.id, light.stable_id);
                area_sum += square.emitter_area;
            }
            // One-sided Lambertian squares of luminance L radiate pi L A: the flux is the light's
            // when their area is its emitting area.
            CY_CHECK_NEAR_REL(area_sum, rendering::area_light_emitting_area(light), 1e-4F);
        }
    }
}

CY_TEST_CASE("gi frame lights: a receiver is lit by the squares as the frame lights it") {
    struct Case {
        render::LightKind kind;
        f32 width;
        f32 height;
        f32 tolerance;
    };
    // The rect's squares tile it exactly; a disc's one square, a sphere's six and a tube's rows
    // are the substitutions, and what each costs a receiver at three metres is the tolerance.
    const Case cases[] = {
        {render::LightKind::Rect, 1.6F, 0.4F, 0.01F},
        {render::LightKind::Disc, 0.8F, 0.0F, 0.02F},
        {render::LightKind::Sphere, 0.5F, 0.0F, 0.05F},
        {render::LightKind::Tube, 1.2F, 0.06F, 0.05F},
    };
    const Vec3 receivers[3] = {Vec3{0.0F, 0.0F, 0.0F}, Vec3{1.0F, 0.0F, 0.5F},
                               Vec3{-1.5F, 0.0F, -1.0F}};
    for (const Case& configuration : cases) {
        const render::LightDescription light =
            area(configuration.kind, configuration.width, configuration.height);
        Array<gi::GiLight> lights(allocator());
        (void)rendering::gi_gpu::append_gi_lights(light, lights);
        for (const Vec3 receiver : receivers) {
            const Vec3 normal{0.0F, 1.0F, 0.0F};
            const Vec3 gi = gi::shaded_direct(lights.span(), receiver, normal, nullptr);
            const Vec3 frame = frame_direct(light, receiver, normal);
            std::printf("  %-7s at (%.1f, %.1f): GI %.3f, frame %.3f (%+.2f%%)\n",
                        render::light_kind_name(configuration.kind), static_cast<f64>(receiver.x),
                        static_cast<f64>(receiver.z), static_cast<f64>(gi.x),
                        static_cast<f64>(frame.x),
                        100.0 * ((static_cast<f64>(gi.x) / static_cast<f64>(frame.x)) - 1.0));
            CY_CHECK_NEAR_REL(gi.x, frame.x, configuration.tolerance);
        }
    }
}

CY_TEST_CASE("gi frame lights: a punctual light is the GI's light of its candela over pi") {
    render::LightDescription point;
    point.kind = render::LightKind::Point;
    point.transform.translation = Vec3{0.0F, 2.0F, 0.0F};
    point.intensity = 1000.0F;  // lumens, the frame's default unit for a point
    point.range = 10.0F;
    point.mobility = render::LightMobility::Static;
    Array<gi::GiLight> lights(allocator());
    CY_REQUIRE_EQ(rendering::gi_gpu::append_gi_lights(point, lights), 1U);
    CY_CHECK_EQ(lights[0].emitter_area, 0.0F);
    CY_CHECK_EQ(lights[0].mobility, gi::LightMobility::Static);
    CY_CHECK_NEAR_REL(lights[0].intensity,
                      rendering::point_candela_from_lumens(1000.0F) / math::kPi, 1e-5F);
    // And it lights a receiver as the frame does, in the GI's units.
    const Vec3 receiver{0.5F, 0.0F, 0.0F};
    const Vec3 normal{0.0F, 1.0F, 0.0F};
    const Vec3 gi = gi::shaded_direct(lights.span(), receiver, normal, nullptr);
    rendering::LightBuildParameters parameters;
    parameters.unit = rendering::default_unit_for(point.kind);
    point.mobility = render::LightMobility::Movable;
    const rendering::GpuLight record = rendering::build_gpu_light(point, parameters);
    // The frame windows the light to zero at its range; at a fifth of it the window is 0.996.
    CY_CHECK_NEAR_REL(gi.x, rendering::light_irradiance(record, receiver, normal).x / math::kPi,
                      5e-3F);
}
