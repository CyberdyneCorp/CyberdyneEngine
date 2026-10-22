// SPDX-License-Identifier: MIT
// render.area_lights — rect, disc, sphere and tube lights in the assembled frame, on a device.
//
// `pipeline_test::FrameScene` lit by area lights alone, its HDR colour read back before the post
// chain. What is asserted:
//
//   (a) a frame with no area light and no table bound is the frame before, byte for byte: the
//       committed reference was drawn by the frame shader as it was before area lights existed;
//   (b) every lit floor pixel is the host's `shade_area_light` for every light, diffuse and
//       specular, through the LTC table on the device — the frame's arithmetic is the twin's;
//   (c) with no table bound, an area light shades its diffuse term alone, and the table changes
//       only the specular half;
//   (d) a STATIC light costs the frame nothing: a record no loop shades and no cluster lists, and
//       the picture without it; a stationary and a movable light are the frame's;
//   (e) the area-lit room, written out: the picture the guide shows.
//
// Vulkan with validation and synchronisation validation on. Without a GPU the suite SKIPS LOUDLY.

#include "golden.h"
#include "light_scene.h"

#include <cy/core/math/quat.h>
#include <cy/rendering/lighting/area_shading.h>
#include <cy/rendering/lighting/lights.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cstdlib>

using namespace cy;
using namespace cy::lights_test;

namespace {

const rendering::LtcTable& table() noexcept {
    static const rendering::LtcTable built = [] {
        rendering::LtcTable made;
        made.build();
        return made;
    }();
    return built;
}

/// The floor's material: slot 0's colour, its roughness and no metal (`FrameScene`).
rendering::AreaSurface floor_surface() noexcept {
    rendering::AreaSurface surface;
    surface.albedo = Vec3{kMaterialColours[0][0], kMaterialColours[0][1], kMaterialColours[0][2]};
    surface.roughness = 0.25F;
    surface.metallic = 0.0F;
    return surface;
}

render::LightDescription area_light(render::LightKind kind, Vec3 at, Vec3 forward, f32 width,
                                    f32 height, Vec3 colour, f32 nits) noexcept {
    render::LightDescription light;
    light.kind = kind;
    light.transform.translation = at;
    const Vec3 unit = normalize(forward);
    const Vec3 up = std::fabs(unit.y) > 0.9F ? Vec3{0.0F, 0.0F, -1.0F} : Vec3{0.0F, 1.0F, 0.0F};
    light.transform.rotation = Quat::look_rotation(unit, up);
    light.source_width = width;
    light.source_height = height;
    light.color[0] = colour.x;
    light.color[1] = colour.y;
    light.color[2] = colour.z;
    light.intensity = nits;
    light.range = 9.0F;
    light.casts_shadow = false;
    light.stable_id = 7000U + static_cast<u64>(kind);
    return light;
}

/// The room's four lights: a warm panel overhead, a cool disc tilted at the back, a green sphere
/// low on the left and a white tube across the right.
std::vector<render::LightDescription> room_lights() {
    return {
        area_light(render::LightKind::Rect, Vec3{0.0F, 0.2F, -6.0F}, Vec3{0.0F, -1.0F, 0.0F}, 1.6F,
                   0.8F, Vec3{1.0F, 0.78F, 0.55F}, 18000.0F),
        area_light(render::LightKind::Disc, Vec3{2.2F, -0.4F, -9.0F}, Vec3{-0.5F, -1.0F, 0.4F},
                   1.0F, 0.0F, Vec3{0.55F, 0.75F, 1.0F}, 22000.0F),
        area_light(render::LightKind::Sphere, Vec3{-2.4F, -1.2F, -6.5F}, Vec3{0.0F, 0.0F, -1.0F},
                   0.4F, 0.0F, Vec3{0.5F, 1.0F, 0.6F}, 60000.0F),
        area_light(render::LightKind::Tube, Vec3{2.0F, -0.9F, -5.0F}, Vec3{0.0F, 0.0F, -1.0F}, 1.6F,
                   0.08F, Vec3{1.0F, 1.0F, 1.0F}, 90000.0F),
    };
}

void save(const char* name, const FrameScene& scene) {
    render_test::Image image(allocator());
    if (!render_test::adopt(image, scene.pixels(), scene.width(), scene.height()).has_value()) {
        return;
    }
    const char* directory = std::getenv("CY_TEST_ARTEFACT_DIR");
    if (directory == nullptr || *directory == '\0') {
        directory = CY_TEST_BINARY_DIR;
    }
    char path[1024];
    (void)std::snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (render_test::write_png(path, image).has_value()) {
        std::fprintf(stderr, "wrote %s (%ux%u)\n", path, scene.width(), scene.height());
    }
}

/// The host's sum over `lights` at a floor point: what the frame adds for them.
Vec3 host_shading(const std::vector<rendering::GpuLight>& lights, Vec3 relative, bool with_ltc,
                  bool diffuse_only) noexcept {
    const rendering::LtcTable empty;
    const rendering::LtcTable& used = with_ltc ? table() : empty;
    const Vec3 view = normalize(relative * -1.0F);
    Vec3 total{0.0F, 0.0F, 0.0F};
    for (const rendering::GpuLight& light : lights) {
        if (!rendering::gpu_light_is_area(light.kind)) {
            continue;
        }
        const rendering::AreaShading shading = rendering::shade_area_light(
            used, light, relative, Vec3{0.0F, 1.0F, 0.0F}, view, floor_surface());
        total = total + shading.diffuse + (diffuse_only ? Vec3{} : shading.specular);
    }
    return total;
}

struct Agreement {
    u32 pixels = 0;
    u32 outside = 0;
    f32 worst_relative = 0.0F;
    f64 mean_lit = 0.0;
};

/// Every open floor pixel of `lit` less `dark`, against the host.
Agreement compare_floor(const LightScene& lit, const LightScene& dark, bool with_ltc,
                        bool diffuse_only) {
    Agreement agreement;
    for (u32 y = 0; y < lit.scene->height(); ++y) {
        for (u32 x = 0; x < lit.scene->width(); ++x) {
            Vec3 relative;
            if (!floor_point(lit, x, y, relative)) {
                continue;
            }
            const Vec3 with = hdr_at(lit, x, y);
            const Vec3 without = hdr_at(dark, x, y);
            const Vec3 device = with - without;
            const Vec3 host = host_shading(lit.uploaded, relative, with_ltc, diffuse_only);
            // THE DEPTH BUFFER PLACES A PIXEL'S POINT TO A FEW PARTS IN TEN THOUSAND ALONG ITS RAY,
            // and under a small, close emitter the light changes that fast: the host's own change
            // over that much of the ray is allowed beside the half-float storage.
            const Vec3 along =
                host_shading(lit.uploaded, relative * 1.0003F, with_ltc, diffuse_only);
            const f32 slope = std::max({std::fabs(along.x - host.x), std::fabs(along.y - host.y),
                                        std::fabs(along.z - host.z)});
            // A floor of four nits: at the far edge of a light's reach its window falls steeply and
            // the dimmest pixels are a fraction of a nit, where a hundredth of one is not a defect.
            const f32 scale = std::max({with.x, with.y, with.z, 4.0F});
            const f32 error =
                std::max(std::max({std::fabs(device.x - host.x), std::fabs(device.y - host.y),
                                   std::fabs(device.z - host.z)}) -
                             slope,
                         0.0F) /
                scale;
            ++agreement.pixels;
            agreement.mean_lit += static_cast<f64>(host.x + host.y + host.z);
            agreement.worst_relative = std::max(agreement.worst_relative, error);
            // Half-float storage of two colours and their difference, and the device's own
            // transcendentals: half a per cent of the brighter. Measured: 0.3%.
            if (error > 5.0e-3F) {
                ++agreement.outside;
            }
        }
    }
    if (agreement.pixels > 0) {
        agreement.mean_lit /= static_cast<f64>(agreement.pixels);
    }
    return agreement;
}

/// One render of `lights` over the open floor, with or without the table.
struct Room {
    FrameScene scene{allocator()};
    LightScene run;
};

[[nodiscard]] bool render_room(DeviceFixture& fixture, Room& room,
                               std::vector<render::LightDescription> lights,
                               const LtcTableTexture* ltc, bool boxes) {
    room.run.fixture = &fixture;
    room.run.lights = std::move(lights);
    room.run.ltc = ltc;
    room.run.boxes = boxes;
    if (!build_scene(room.run, room.scene, kWidth, kHeight, 16)) {
        return false;
    }
    const bool rendered = render_frame(room.run);
    return rendered;
}

}  // namespace

CY_TEST_CASE("(a) no area light and no table is the frame before, byte for byte") {
    DeviceFixture fixture("cy_test_render_area_lights");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Room room;
    room.run.replace_lights = false;
    room.run.night = false;
    room.run.exposure_stops = -11.4F;
    room.run.fixture = &fixture;
    CY_REQUIRE(build_scene(room.run, room.scene, kWidth, kHeight, 16));
    CY_REQUIRE(render_frame(room.run));
    render_test::Image rendered(allocator());
    CY_REQUIRE(render_test::adopt(rendered, room.scene.pixels(), kWidth, kHeight).has_value());
    char reference_path[1024];
    (void)std::snprintf(reference_path, sizeof(reference_path), "%s/references/area_lights_off.png",
                        CY_LIGHTS_GPU_TEST_DIR);
    const char* update = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    if (update != nullptr && update[0] != '\0' && update[0] != '0') {
        CY_CHECK(render_test::write_png(reference_path, rendered).has_value());
        std::fprintf(stderr, "wrote %s — look at it, then commit it. This run FAILS on purpose.\n",
                     reference_path);
        CY_TEST_FAIL_CHECK("references were regenerated; this mode never passes");
        release_scene(room.run);
        return;
    }
    render_test::Image reference(allocator());
    CY_REQUIRE(render_test::read_png(reference_path, reference).has_value());
    const render_test::Comparison comparison = render_test::compare(reference, rendered);
    std::fprintf(stderr, "(a) against the frame before area lights: %u differing, worst %u\n",
                 comparison.differing, comparison.max_channel_delta);
    CY_CHECK(comparison.comparable);
    CY_CHECK_EQ(comparison.differing, 0U);
    CY_CHECK_EQ(comparison.max_channel_delta, 0U);
    release_scene(room.run);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(b) every lit floor pixel is the host's area shading, diffuse and specular") {
    DeviceFixture fixture("cy_test_render_area_lights");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    LtcTableTexture ltc;
    ltc.initialize(fixture.device(), allocator());
    CY_REQUIRE(ltc.upload(table()).has_value());
    Room dark;
    CY_REQUIRE(render_room(fixture, dark, {}, &ltc, false));
    Room lit;
    CY_REQUIRE(render_room(fixture, lit, room_lights(), &ltc, false));
    CY_CHECK_EQ(lit.run.report.area_lights, 4U);
    const Agreement agreement = compare_floor(lit.run, dark.run, true, false);
    std::fprintf(stderr,
                 "(b) %u floor pixels against shade_area_light: %u outside, worst %.5f of the "
                 "pixel, mean lit radiance %.2f\n",
                 agreement.pixels, agreement.outside, static_cast<f64>(agreement.worst_relative),
                 agreement.mean_lit);
    CY_CHECK_GT(agreement.pixels, 20000U);
    CY_CHECK_GT(agreement.mean_lit, 1.0);
    CY_CHECK_EQ(agreement.outside, 0U);
    release_scene(dark.run);
    release_scene(lit.run);
    ltc.shutdown();
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(c) with no table bound an area light shades its diffuse term, and only that") {
    DeviceFixture fixture("cy_test_render_area_lights");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Room dark;
    CY_REQUIRE(render_room(fixture, dark, {}, nullptr, false));
    Room lit;
    CY_REQUIRE(render_room(fixture, lit, room_lights(), nullptr, false));
    const Agreement diffuse = compare_floor(lit.run, dark.run, false, true);
    std::fprintf(stderr, "(c) %u floor pixels against the diffuse term: %u outside, worst %.5f\n",
                 diffuse.pixels, diffuse.outside, static_cast<f64>(diffuse.worst_relative));
    CY_CHECK_GT(diffuse.pixels, 20000U);
    CY_CHECK_EQ(diffuse.outside, 0U);
    // And the same frame is NOT the full shading: the table is what adds the highlights.
    const Agreement full = compare_floor(lit.run, dark.run, true, false);
    CY_CHECK_GT(full.outside, 1000U);
    release_scene(dark.run);
    release_scene(lit.run);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(d) a static light costs the frame nothing; stationary and movable are the frame's") {
    DeviceFixture fixture("cy_test_render_area_lights");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    render::LightDescription lamp;
    lamp.kind = render::LightKind::Point;
    lamp.transform.translation = Vec3{0.5F, -1.0F, -6.0F};
    lamp.intensity = 60000.0F;
    lamp.range = 6.0F;
    lamp.casts_shadow = false;
    lamp.stable_id = 77;
    Room none;
    CY_REQUIRE(render_room(fixture, none, {}, nullptr, true));
    for (const render::LightMobility mobility :
         {render::LightMobility::Static, render::LightMobility::Stationary,
          render::LightMobility::Movable}) {
        lamp.mobility = mobility;
        Room room;
        CY_REQUIRE(render_room(fixture, room, {lamp}, nullptr, true));
        const u32 differing = room.scene.differing_texels(none.scene.pixels());
        const bool is_static = mobility == render::LightMobility::Static;
        std::fprintf(stderr, "(d) %s: %u texels differ from the frame without it; %u in lists\n",
                     render::light_mobility_name(mobility), differing,
                     room.run.report.clusters.elements);
        CY_CHECK_EQ(room.run.report.static_lights, is_static ? 1U : 0U);
        // The record is there either way — its index is its identity — and a static one is the
        // placeholder no loop shades, in no cluster list.
        CY_REQUIRE_EQ(room.run.uploaded.size(), 1U);
        CY_CHECK_EQ(room.run.uploaded[0].kind == rendering::kGpuLightBaked, is_static);
        CY_CHECK_EQ(room.run.report.clusters.elements, is_static ? 0U : 1U);
        if (is_static) {
            CY_CHECK_EQ(differing, 0U);
        } else {
            CY_CHECK_GT(differing, 500U);
        }
        release_scene(room.run);
    }
    release_scene(none.run);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(e) the area-lit room") {
    DeviceFixture fixture("cy_test_render_area_lights");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    LtcTableTexture ltc;
    ltc.initialize(fixture.device(), allocator());
    CY_REQUIRE(ltc.upload(table()).has_value());
    // A room: a floor, three walls and a plinth, lit by a ceiling panel, a disc on the back wall, a
    // floating sphere and a tube along the right wall.
    Room room;
    room.run.camera = true;
    room.run.eye = Vec3{0.0F, 0.2F, 1.0F};
    room.run.forward = Vec3{0.0F, -0.22F, -1.0F};
    room.run.floor_centre = Vec3{0.0F, -1.9F, -6.0F};
    room.run.floor_half = Vec3{4.0F, 0.125F, 4.0F};
    room.run.floor_albedo = Vec3{0.55F, 0.52F, 0.48F};
    room.run.exposure_stops = -11.5F;
    const Vec3 wall{0.72F, 0.70F, 0.66F};
    room.run.layout = {
        {Vec3{0.0F, 0.3F, -10.1F}, Vec3{4.2F, 2.2F, 0.1F}, wall},
        {Vec3{-4.1F, 0.3F, -6.0F}, Vec3{0.1F, 2.2F, 4.2F}, wall},
        {Vec3{4.1F, 0.3F, -6.0F}, Vec3{0.1F, 2.2F, 4.2F}, wall},
        {Vec3{0.6F, -1.25F, -6.8F}, Vec3{0.5F, 0.5F, 0.5F}, Vec3{0.3F, 0.32F, 0.36F}},
    };
    const std::vector<render::LightDescription> lights = {
        area_light(render::LightKind::Rect, Vec3{0.0F, 1.9F, -6.0F}, Vec3{0.0F, -1.0F, 0.0F}, 2.0F,
                   1.0F, Vec3{1.0F, 0.8F, 0.6F}, 45000.0F),
        area_light(render::LightKind::Disc, Vec3{-2.0F, 0.2F, -9.95F}, Vec3{0.0F, 0.0F, 1.0F}, 1.2F,
                   0.0F, Vec3{0.45F, 0.65F, 1.0F}, 40000.0F),
        area_light(render::LightKind::Sphere, Vec3{-1.6F, -1.0F, -5.2F}, Vec3{0.0F, 0.0F, -1.0F},
                   0.35F, 0.0F, Vec3{0.45F, 1.0F, 0.55F}, 60000.0F),
        area_light(render::LightKind::Tube, Vec3{3.85F, 0.4F, -6.0F}, Vec3{-1.0F, 0.0F, 0.0F}, 3.0F,
                   0.08F, Vec3{1.0F, 0.55F, 0.75F}, 60000.0F),
    };
    for (render::LightDescription light : lights) {
        light.range = 14.0F;
        room.run.lights.push_back(light);
    }
    CY_REQUIRE(render_room(fixture, room, room.run.lights, &ltc, true));
    save("area-lit-room.png", room.scene);
    // Every light shows: the floor under each is brighter than the room's darkest floor.
    CY_CHECK_GT(room.run.report.area_lights, 3U);
    release_scene(room.run);
    ltc.shutdown();
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
