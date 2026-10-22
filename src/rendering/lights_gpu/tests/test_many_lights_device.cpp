// SPDX-License-Identifier: MIT
// render.many_lights — hundreds to thousands of lights on a device: the clustered lists assigned on
// the device, and the stochastic many-light path.
//
//   (a) the device's lists are the host's, less exactly the lights whose bounds miss the tile's
//       frustum, and the overflow is counted;
//   (b) and they draw the host's picture, byte for byte;
//   (c) a thousand lights at 1080p: the assignment and the lighting inside a stated budget;
//   (d) the stochastic estimate is unbiased against a sum over every light;
//   (e) with temporal and spatial reuse its bias stays inside a stated bound;
//   (f) its denoiser is the engine's: `Denoiser::denoise` over the device's own noisy signal and
//       reprojection gives the device's picture;
//   (g) converged, the denoised signal's noise and its frame-to-frame change are inside stated
//       bounds, and far below the raw signal's;
//   (h) the same content under either path: the two pictures agree where nothing is shadowed;
//   (i) an area light's shadow is soft: the stochastic path's visibility darkens the floor under a
//       box, through a penumbra;
//   (j) the hundreds of coloured lights, written out;
//   (k) the path is the frame arbiter's choice: a frame whose clusters drop lights at their bound
//       leaves for the stochastic path at once, and returns after the dwell once the clustered
//       lighting fits the arbiter's shading allocation again;
//   (l) past its reach a light adds exactly zero: the cluster loop, which skips it, draws the
//       frame the walk over every light draws.
//
// Vulkan with validation and synchronisation validation on. Without a GPU the suite SKIPS LOUDLY.

#include "golden.h"
#include "light_scene.h"

#include <cy/core/math/quat.h>
#include <cy/rendering/arbiter/profiles.h>
#include <cy/rendering/denoise/denoiser.h>
#include <cy/rendering/lighting/area_shading.h>
#include <cy/rendering/lighting/lights.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cstdlib>

using namespace cy;
using namespace cy::lights_test;

namespace {

/// A deterministic stream, so every run places the same lights.
struct Stream {
    u32 state = 0x9E3779B9U;
    f32 next() noexcept {
        state = (state * 1664525U) + 1013904223U;
        return static_cast<f32>(state >> 8U) / static_cast<f32>(1U << 24U);
    }
};

Vec3 hue(f32 h) noexcept {
    const f32 r = std::clamp(std::fabs((h * 6.0F) - 3.0F) - 1.0F, 0.0F, 1.0F);
    const f32 g = std::clamp(2.0F - std::fabs((h * 6.0F) - 2.0F), 0.0F, 1.0F);
    const f32 b = std::clamp(2.0F - std::fabs((h * 6.0F) - 4.0F), 0.0F, 1.0F);
    return Vec3{0.15F + (0.85F * r), 0.15F + (0.85F * g), 0.15F + (0.85F * b)};
}

/// Where the lights are scattered: over a floor of this centre and half extent.
struct Spread {
    Vec3 centre{0.0F, -1.9F, -8.0F};
    f32 half = 5.5F;
};

/// `count` coloured lights over the floor: points, a few spots, and every `area_every`-th one a
/// small area light of each shape in turn.
std::vector<render::LightDescription> coloured_lights(u32 count, u32 area_every, f32 range,
                                                      f32 candela, Spread spread = {}) {
    std::vector<render::LightDescription> lights;
    Stream stream;
    const f32 top = spread.centre.y + 0.125F;
    for (u32 index = 0; index < count; ++index) {
        render::LightDescription light;
        light.kind = render::LightKind::Point;
        light.transform.translation =
            Vec3{spread.centre.x - spread.half + (2.0F * spread.half * stream.next()),
                 top + 0.15F + (1.4F * stream.next()),
                 spread.centre.z - spread.half + (2.0F * spread.half * stream.next())};
        const Vec3 colour = hue(stream.next());
        light.color[0] = colour.x;
        light.color[1] = colour.y;
        light.color[2] = colour.z;
        light.intensity = candela * (0.5F + stream.next());
        light.range = range * (0.6F + (0.8F * stream.next()));
        light.casts_shadow = false;
        light.stable_id = 10000U + index;
        if (index % 9U == 4U) {
            light.kind = render::LightKind::Spot;
            light.transform.rotation =
                Quat::from_axis_angle(Vec3{1.0F, 0.0F, 0.0F}, -0.5F * math::kPi);
            light.inner_cone_radians = 0.3F;
            light.outer_cone_radians = 0.7F;
        }
        if (area_every != 0 && index % area_every == 0U) {
            const u32 shape = (index / area_every) % 4U;
            const render::LightKind kinds[4] = {render::LightKind::Rect, render::LightKind::Disc,
                                                render::LightKind::Sphere, render::LightKind::Tube};
            light.kind = kinds[shape];
            light.transform.rotation =
                Quat::from_axis_angle(Vec3{1.0F, 0.0F, 0.0F}, -0.5F * math::kPi);
            light.source_width = 0.3F;
            light.source_height = shape == 3 ? 0.04F : 0.2F;
            light.intensity = candela * 40.0F;  // nits: a small, bright panel
        }
        lights.push_back(light);
    }
    return lights;
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

struct Frame {
    FrameScene scene{allocator()};
    LightScene run;
};

[[nodiscard]] bool build(DeviceFixture& fixture, Frame& frame,
                         std::vector<render::LightDescription> lights, bool boxes, u32 width,
                         u32 height) {
    frame.run.fixture = &fixture;
    frame.run.lights = std::move(lights);
    frame.run.boxes = boxes;
    frame.run.exposure_stops = -8.0F;
    return build_scene(frame.run, frame.scene, width, height,
                       static_cast<u32>(frame.run.lights.size()) + 4U);
}

/// Whether a light's bounding sphere reaches tile (x, y)'s frustum — light_assignment.slang's
/// `sphereInTile`, on the host.
bool sphere_in_tile(const rendering::ClusterGrid& grid, u32 x, u32 y, f32 tan_y, f32 aspect,
                    const rendering::ClusterElement& element) {
    const f32 tan_x = tan_y * aspect;
    const f32 x0 =
        (((static_cast<f32>(x) / static_cast<f32>(grid.dimensions[0])) * 2.0F) - 1.0F) * tan_x;
    const f32 x1 =
        (((static_cast<f32>(x + 1U) / static_cast<f32>(grid.dimensions[0])) * 2.0F) - 1.0F) * tan_x;
    const f32 y1 =
        (1.0F - ((static_cast<f32>(y) / static_cast<f32>(grid.dimensions[1])) * 2.0F)) * tan_y;
    const f32 y0 =
        (1.0F - ((static_cast<f32>(y + 1U) / static_cast<f32>(grid.dimensions[1])) * 2.0F)) * tan_y;
    const Vec3 p = element.view_position;
    const f32 r = element.radius;
    const Vec3 planes[4] = {Vec3{1.0F, 0.0F, x0}, Vec3{-1.0F, 0.0F, -x1}, Vec3{0.0F, 1.0F, y0},
                            Vec3{0.0F, -1.0F, -y1}};
    // A sliver of slack for the device's own rounding at the plane.
    return std::ranges::all_of(planes, [&](const Vec3 plane) {
        return dot(p, plane) >= (-r * length(plane)) - 1e-4F;
    });
}

f64 luminance(Vec3 value) noexcept {
    return (0.2126 * static_cast<f64>(value.x)) + (0.7152 * static_cast<f64>(value.y)) +
           (0.0722 * static_cast<f64>(value.z));
}

/// The stochastic path's settings for a case.
ManyLightsSettings settings_for(bool temporal, bool spatial, bool visibility) noexcept {
    ManyLightsSettings settings;
    settings.path.enabled = true;
    settings.path.denoising_available = true;
    settings.path.candidates_per_pixel = 16;
    settings.path.spatial_neighbours = 4;
    settings.path.max_history_samples = 320;
    settings.temporal_reuse = temporal;
    settings.spatial_reuse = spatial;
    settings.visibility = visibility;
    return settings;
}

}  // namespace

CY_TEST_CASE("(a) the device's lists are the host's, less the lights that miss the tile") {
    DeviceFixture fixture("cy_test_render_many_lights");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    const std::vector<render::LightDescription> lights = coloured_lights(240, 0, 1.2F, 900.0F);
    Frame host;
    CY_REQUIRE(build(fixture, host, lights, true, kWidth, kHeight));
    CY_REQUIRE(render_frame(host.run));
    const rendering::ClusterAssignment& host_lists = host.scene.assembly().clusters();
    std::vector<rendering::ClusterHeader> host_headers(host_lists.headers.begin(),
                                                       host_lists.headers.end());
    std::vector<u32> host_indices(host_lists.indices.begin(), host_lists.indices.end());

    Frame device;
    device.run.fixture = &fixture;
    device.run.lights = lights;
    device.run.exposure_stops = -8.0F;
    // The elements the device was given, kept for the frustum check below.
    GpuLightAssignment assignment;
    Expected<rendering::ClusterGrid, Error> grid = rendering::make_cluster_grid(
        rendering::ClusterGridConfig{32, 24, 64}, kWidth, kHeight, 0.1F, 120.0F);
    CY_REQUIRE(grid.has_value());
    CY_REQUIRE(assignment
                   .create(allocator(), fixture.device(),
                           LightAssignmentDescription{*grid, 1024, 65536, true})
                   .has_value());
    device.run.assignment = &assignment;
    CY_REQUIRE(build_scene(device.run, device.scene, kWidth, kHeight, 400));
    CY_REQUIRE(render_frame(device.run));
    const std::vector<rendering::ClusterElement> elements(
        device.scene.assembly().light_elements().begin(),
        device.scene.assembly().light_elements().end());
    CY_REQUIRE_EQ(elements.size(), lights.size());

    std::vector<rendering::ClusterHeader> headers(usize{grid->cluster_count()} *
                                                  rendering::kClusterElementTypeCount);
    std::vector<u32> indices(65536U +
                             (usize{grid->cluster_count()} * grid->max_elements_per_cluster));
    CY_REQUIRE(
        assignment.read_back({headers.data(), headers.size()}, {indices.data(), indices.size()})
            .has_value());

    const f32 tan_y = std::tan(0.45F);
    const f32 aspect = static_cast<f32>(kWidth) / static_cast<f32>(kHeight);
    u32 clusters_equal = 0;
    u32 extra_on_device = 0;
    u32 missing_but_reaching = 0;
    u32 dropped_by_frustum = 0;
    u64 host_total = 0;
    u64 device_total = 0;
    for (u32 slice = 0; slice < grid->dimensions[2]; ++slice) {
        for (u32 y = 0; y < grid->dimensions[1]; ++y) {
            for (u32 x = 0; x < grid->dimensions[0]; ++x) {
                const u32 cluster = rendering::cluster_index_of(*grid, x, y, slice);
                const rendering::ClusterHeader host_header =
                    host_headers[usize{cluster} * rendering::kClusterElementTypeCount];
                const rendering::ClusterHeader device_header =
                    headers[usize{cluster} * rendering::kClusterElementTypeCount];
                const std::vector<u32> on_host(
                    host_indices.begin() + host_header.offset,
                    host_indices.begin() + host_header.offset + host_header.count);
                const std::vector<u32> on_device(
                    indices.begin() + device_header.offset,
                    indices.begin() + device_header.offset + device_header.count);
                host_total += on_host.size();
                device_total += on_device.size();
                clusters_equal += on_host == on_device ? 1U : 0U;
                for (const u32 light : on_device) {
                    extra_on_device +=
                        std::ranges::find(on_host, light) == on_host.end() ? 1U : 0U;
                }
                for (const u32 light : on_host) {
                    if (std::ranges::find(on_device, light) != on_device.end()) {
                        continue;
                    }
                    if (sphere_in_tile(*grid, x, y, tan_y, aspect, elements[light])) {
                        ++missing_but_reaching;
                    } else {
                        ++dropped_by_frustum;
                    }
                }
            }
        }
    }
    const AssignmentStatistics statistics = assignment.statistics();
    std::fprintf(stderr,
                 "(a) %u clusters: %u identical; host %llu assignments, device %llu (%u fewer, "
                 "every one outside the tile's frustum); device overflow %u, longest list %u\n",
                 grid->cluster_count(), clusters_equal, static_cast<unsigned long long>(host_total),
                 static_cast<unsigned long long>(device_total), dropped_by_frustum,
                 statistics.overflow, statistics.max_per_cluster);
    CY_CHECK_EQ(host.run.report.clusters.overflow, 0U);
    CY_CHECK_EQ(statistics.overflow, 0U);
    CY_CHECK_EQ(extra_on_device, 0U);
    CY_CHECK_EQ(missing_but_reaching, 0U);
    CY_CHECK_EQ(statistics.assignments, static_cast<u32>(device_total));
    CY_CHECK_GT(device_total, 1000U);
    CY_CHECK_GT(clusters_equal, grid->cluster_count() / 2U);

    // (b) — the picture is the host's, byte for byte: a light the device left out of a tile adds
    // exactly zero to every pixel of it.
    const u32 differing = device.scene.differing_texels(host.scene.pixels());
    u32 hdr_differing = 0;
    for (usize index = 0; index < host.run.hdr.size(); ++index) {
        hdr_differing += host.run.hdr[index] != device.run.hdr[index] ? 1U : 0U;
    }
    std::fprintf(stderr,
                 "(b) device lists against host lists: %u texels and %u HDR halves differ\n",
                 differing, hdr_differing);
    CY_CHECK_EQ(differing, 0U);
    CY_CHECK_EQ(hdr_differing, 0U);
    release_scene(host.run);
    release_scene(device.run);
    assignment.destroy();
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(c) a thousand lights at 1080p: the assignment and the lighting inside the budget") {
    DeviceFixture fixture("cy_test_render_many_lights");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    constexpr u32 kWide = 1920;
    constexpr u32 kTall = 1080;
    constexpr u32 kLights = 1000;
    // A plaza forty metres square seen from a few metres up, a thousand lights over it: the scene
    // a city street at night or a battlefield of muzzle flashes is.
    GpuLightAssignment assignment;
    Frame frame;
    frame.run.fixture = &fixture;
    frame.run.floor_centre = Vec3{0.0F, -1.9F, -22.0F};
    frame.run.floor_half = Vec3{20.0F, 0.125F, 20.0F};
    frame.run.camera = true;
    frame.run.eye = Vec3{0.0F, 3.5F, 0.0F};
    frame.run.forward = Vec3{0.0F, -0.32F, -1.0F};
    frame.run.lights =
        coloured_lights(kLights, 10, 1.6F, 900.0F, Spread{Vec3{0.0F, -1.9F, -22.0F}, 19.0F});
    frame.run.exposure_stops = -10.5F;
    frame.run.boxes = false;
    frame.run.floor_albedo = Vec3{0.5F, 0.5F, 0.5F};
    frame.run.assignment = &assignment;
    CY_REQUIRE(build_scene(frame.run, frame.scene, kWide, kTall, kLights + 4U));
    // The device's grid is the frame's: the assembly's own configuration at this extent.
    const rendering::assembly::AssemblyDescription& assembled =
        frame.scene.assembly().description();
    Expected<rendering::ClusterGrid, Error> grid = rendering::make_cluster_grid(
        assembled.clusters, kWide, kTall, assembled.near_plane, assembled.far_plane);
    CY_REQUIRE(grid.has_value());
    CY_REQUIRE(assignment
                   .create(allocator(), fixture.device(),
                           LightAssignmentDescription{*grid, 4096, 65536, false})
                   .has_value());
    // The first frames build pipelines and fill caches; the budget is the steady state's.
    f64 assign_ms = 1.0e9;
    f64 lighting_ms = 1.0e9;
    f64 frame_ms = 1.0e9;
    for (u32 round = 0; round < 6; ++round) {
        CY_REQUIRE(render_frame(frame.run));
        if (round < 2) {
            continue;
        }
        assign_ms = std::min(assign_ms, assignment.last_device_ms());
        lighting_ms = std::min(
            lighting_ms, frame.run.stamps_ms[kStampOpaque] - frame.run.stamps_ms[kStampPrepass]);
        frame_ms = std::min(frame_ms, frame.run.stamps_ms[kStampPost]);
    }
    const AssignmentStatistics statistics = assignment.statistics();
    std::fprintf(stderr,
                 "(c) %u lights at %ux%u: device assignment %.3f ms (%u assignments, longest list "
                 "%u, overflow %u); prepass end to opaque end %.3f ms; frame to post-process %.3f "
                 "ms\n",
                 kLights, kWide, kTall, assign_ms, statistics.assignments,
                 statistics.max_per_cluster, statistics.overflow, lighting_ms, frame_ms);
    // THE BUDGET, on this suite's reference device (an RTX 5060): a thousand dynamic lights cost
    // the frame under half a millisecond to assign and under a millisecond and a half from the end
    // of the depth prepass to the end of the opaque pass — the assignment and the whole lit shading
    // of 1080p. Measured: 0.19 ms and 0.70 ms.
    CY_CHECK_GT(assign_ms, 0.0);
    CY_CHECK_LT(assign_ms, 0.5);
    CY_CHECK_LT(lighting_ms, 1.5);
    CY_CHECK_EQ(statistics.overflow, 0U);
    save("many-lights-1080p.png", frame.scene);
    release_scene(frame.run);
    assignment.destroy();
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

namespace {

/// Render `frames` frames of the stochastic path over an open floor and average its noisy signal.
struct Estimate {
    std::vector<Vec3> mean;
    std::vector<Vec4> surface;
    u32 frames = 0;
};

[[nodiscard]] bool estimate(DeviceFixture& fixture,
                            const std::vector<render::LightDescription>& lights,
                            const ManyLightsSettings& settings, u32 frames, Estimate& out,
                            std::vector<rendering::GpuLight>& records, Vec2& jitter) {
    ManyLightsPass pass;
    if (Status made = pass.create(allocator(), fixture.device(),
                                  ManyLightsPassDescription{kWidth, kHeight, 1024, true}, settings);
        !made) {
        std::fprintf(stderr, "many lights: %s\n", made.error().message);
        return false;
    }
    Frame frame;
    frame.run.fixture = &fixture;
    frame.run.lights = lights;
    frame.run.boxes = false;
    frame.run.many = &pass;
    frame.run.exposure_stops = -8.0F;
    if (!build_scene(frame.run, frame.scene, kWidth, kHeight,
                     static_cast<u32>(lights.size()) + 4U)) {
        return false;
    }
    const usize pixels = usize{kWidth} * kHeight;
    out.mean.assign(pixels, Vec3{});
    ManyLightsReadback readback;
    for (u32 round = 0; round < frames; ++round) {
        if (!render_frame(frame.run) || !pass.read_back(readback)) {
            return false;
        }
        for (usize pixel = 0; pixel < pixels; ++pixel) {
            const Vec4 value = readback.noisy[pixel];
            out.mean[pixel] = out.mean[pixel] + Vec3{value.x, value.y, value.z};
        }
    }
    for (Vec3& value : out.mean) {
        value = value * (1.0F / static_cast<f32>(frames));
    }
    out.surface.assign(readback.surface.begin(), readback.surface.end());
    out.frames = frames;
    records = frame.run.uploaded;
    jitter = frame.run.report.jitter;
    release_scene(frame.run);
    pass.destroy();
    return true;
}

struct Bias {
    f64 relative = 0.0;
    u32 pixels = 0;
    f64 within_ten_percent = 0.0;
};

/// The estimate's mean against the sum over every light at each open floor pixel.
Bias bias_of(const Estimate& estimate, const std::vector<rendering::GpuLight>& records,
             Vec2 jitter) {
    FrameScene geometry(allocator());
    LightScene probe;
    probe.scene = &geometry;
    probe.report.jitter = jitter;
    geometry.set_extent(kWidth, kHeight);
    Bias bias;
    f64 estimated = 0.0;
    f64 exact = 0.0;
    u32 close = 0;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            Vec3 relative;
            if (!floor_point(probe, x, y, relative)) {
                continue;
            }
            const usize pixel = (static_cast<usize>(y) * kWidth) + x;
            if (estimate.surface[pixel].x <= 0.0F) {
                continue;
            }
            Vec3 sum{};
            for (const rendering::GpuLight& light : records) {
                sum = sum + rendering::light_irradiance(light, relative, Vec3{0.0F, 1.0F, 0.0F});
            }
            const f64 truth = luminance(sum);
            const f64 measured = luminance(estimate.mean[pixel]);
            if (truth <= 1e-3) {
                continue;
            }
            estimated += measured;
            exact += truth;
            close += std::fabs((measured / truth) - 1.0) < 0.1 ? 1U : 0U;
            ++bias.pixels;
        }
    }
    bias.relative = exact > 0.0 ? (estimated / exact) - 1.0 : 0.0;
    bias.within_ten_percent =
        bias.pixels > 0 ? static_cast<f64>(close) / static_cast<f64>(bias.pixels) : 0.0;
    return bias;
}

}  // namespace

CY_TEST_CASE("(d) the stochastic estimate is unbiased against a sum over every light") {
    DeviceFixture fixture("cy_test_render_many_lights");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    const std::vector<render::LightDescription> lights = coloured_lights(200, 7, 2.5F, 900.0F);
    Estimate result;
    std::vector<rendering::GpuLight> records;
    Vec2 jitter{};
    CY_REQUIRE(
        estimate(fixture, lights, settings_for(false, false, false), 48, result, records, jitter));
    const Bias bias = bias_of(result, records, jitter);
    std::fprintf(stderr,
                 "(d) %u floor pixels, %u frames, 16 candidates, no reuse: relative bias %+.4f%%; "
                 "%.1f%% of pixels within 10%% of the sum\n",
                 bias.pixels, result.frames, 100.0 * bias.relative,
                 100.0 * bias.within_ten_percent);
    CY_CHECK_GT(bias.pixels, 20000U);
    CY_CHECK_LT(std::fabs(bias.relative), 0.01);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(e) with temporal and spatial reuse the estimate's bias stays inside its bound") {
    DeviceFixture fixture("cy_test_render_many_lights");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    const std::vector<render::LightDescription> lights = coloured_lights(200, 7, 2.5F, 900.0F);
    Estimate result;
    std::vector<rendering::GpuLight> records;
    Vec2 jitter{};
    CY_REQUIRE(
        estimate(fixture, lights, settings_for(true, true, false), 48, result, records, jitter));
    const Bias bias = bias_of(result, records, jitter);
    std::fprintf(stderr,
                 "(e) with reuse: relative bias %+.4f%%; %.1f%% of pixels within 10%% of the sum\n",
                 100.0 * bias.relative, 100.0 * bias.within_ten_percent);
    CY_CHECK_LT(std::fabs(bias.relative), 0.01);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

namespace {

/// A stochastic frame sequence over one scene: the pass, the scene, and the readbacks.
struct Stochastic {
    ManyLightsPass pass;
    Frame frame;
    ManyLightsReadback readback;
};

[[nodiscard]] bool start(DeviceFixture& fixture, Stochastic& run,
                         std::vector<render::LightDescription> lights, bool boxes,
                         const ManyLightsSettings& settings) {
    if (Status made =
            run.pass.create(allocator(), fixture.device(),
                            ManyLightsPassDescription{kWidth, kHeight, 1024, true}, settings);
        !made) {
        std::fprintf(stderr, "many lights: %s\n", made.error().message);
        return false;
    }
    run.frame.run.fixture = &fixture;
    run.frame.run.lights = std::move(lights);
    run.frame.run.boxes = boxes;
    run.frame.run.many = &run.pass;
    run.frame.run.exposure_stops = -11.0F;
    run.frame.run.floor_albedo = Vec3{0.5F, 0.5F, 0.5F};
    return build_scene(run.frame.run, run.frame.scene, kWidth, kHeight,
                       static_cast<u32>(run.frame.run.lights.size()) + 4U);
}

[[nodiscard]] bool step(Stochastic& run) {
    return render_frame(run.frame.run) && run.pass.read_back(run.readback).has_value();
}

void stop(Stochastic& run) {
    release_scene(run.frame.run);
    run.pass.destroy();
}

}  // namespace

CY_TEST_CASE(
    "(f) the path's denoiser is the engine's: Denoiser::denoise over the device's signal") {
    DeviceFixture fixture("cy_test_render_many_lights");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Stochastic run;
    CY_REQUIRE(start(fixture, run, coloured_lights(160, 8, 2.5F, 900.0F), false,
                     settings_for(true, true, false)));
    denoise::Denoiser host;
    CY_REQUIRE(host.resize(kWidth, kHeight).has_value());
    const usize pixels = usize{kWidth} * kHeight;
    std::vector<Vec3> values(pixels);
    std::vector<f32> depth(pixels);
    std::vector<Vec3> normals(pixels);
    std::vector<i32> source(pixels);
    std::vector<f32> confidence(pixels);
    f64 worst = 0.0;
    u32 compared = 0;
    u32 outside = 0;
    for (u32 frame = 0; frame < 3; ++frame) {
        CY_REQUIRE(step(run));
        const ManyLightsReadback& device = run.readback;
        for (usize pixel = 0; pixel < pixels; ++pixel) {
            values[pixel] =
                Vec3{device.noisy[pixel].x, device.noisy[pixel].y, device.noisy[pixel].z};
            depth[pixel] = device.surface[pixel].x;
            normals[pixel] =
                Vec3{device.surface[pixel].y, device.surface[pixel].z, device.surface[pixel].w};
            source[pixel] = static_cast<i32>(device.guidance[pixel * 2U]);
            confidence[pixel] = device.guidance[(pixel * 2U) + 1U];
        }
        denoise::NoisySignal noisy;
        noisy.values = {values.data(), pixels};
        denoise::GuidanceBuffers guidance;
        guidance.width = kWidth;
        guidance.height = kHeight;
        guidance.depth = {depth.data(), pixels};
        guidance.normal = {normals.data(), pixels};
        denoise::HistoryGuidance history;
        if (frame > 0) {
            history.source = {source.data(), pixels};
            history.confidence = {confidence.data(), pixels};
        }
        const auto denoised =
            host.denoise(denoise::SignalKind::StochasticDirect, noisy, guidance, history);
        CY_REQUIRE(denoised.has_value());
        for (usize pixel = 0; pixel < pixels; ++pixel) {
            if (depth[pixel] <= 0.0F) {
                continue;
            }
            const Vec3 expected = (*denoised)[pixel];
            const Vec4 measured = device.irradiance[pixel];
            const f64 scale = std::max(luminance(expected), 1.0);
            const f64 error = std::fabs(luminance(Vec3{measured.x, measured.y, measured.z}) -
                                        luminance(expected)) /
                              scale;
            worst = std::max(worst, error);
            outside += error > 4.0e-3 ? 1U : 0U;
            ++compared;
        }
    }
    std::fprintf(stderr,
                 "(f) %u pixels over 3 frames against Denoiser::denoise: %u outside, worst %.5f\n",
                 compared, outside, worst);
    CY_CHECK_GT(compared, 60000U);
    CY_CHECK_EQ(outside, 0U);
    stop(run);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(g) converged, the denoised signal is quiet and steady, and far quieter than raw") {
    DeviceFixture fixture("cy_test_render_many_lights");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Stochastic run;
    CY_REQUIRE(start(fixture, run, coloured_lights(160, 8, 2.5F, 900.0F), false,
                     settings_for(true, true, true)));
    for (u32 frame = 0; frame < 30; ++frame) {
        CY_REQUIRE(step(run));
    }
    const usize pixels = usize{kWidth} * kHeight;
    constexpr u32 kFrames = 16;
    std::vector<f64> sum(pixels, 0.0);
    std::vector<f64> squares(pixels, 0.0);
    std::vector<f64> raw_sum(pixels, 0.0);
    std::vector<f64> raw_squares(pixels, 0.0);
    std::vector<f64> previous(pixels, 0.0);
    f64 change = 0.0;
    f64 level = 0.0;
    for (u32 frame = 0; frame < kFrames; ++frame) {
        CY_REQUIRE(step(run));
        for (usize pixel = 0; pixel < pixels; ++pixel) {
            const Vec4 denoised = run.readback.irradiance[pixel];
            const f64 value = luminance(Vec3{denoised.x, denoised.y, denoised.z});
            const Vec4 noisy = run.readback.noisy[pixel];
            const f64 raw = luminance(Vec3{noisy.x, noisy.y, noisy.z});
            sum[pixel] += value;
            squares[pixel] += value * value;
            raw_sum[pixel] += raw;
            raw_squares[pixel] += raw * raw;
            if (frame > 0) {
                change += std::fabs(value - previous[pixel]);
                level += value;
            }
            previous[pixel] = value;
        }
    }
    f64 relative_deviation = 0.0;
    f64 raw_relative_deviation = 0.0;
    u32 counted = 0;
    for (usize pixel = 0; pixel < pixels; ++pixel) {
        const f64 mean = sum[pixel] / kFrames;
        if (run.readback.surface[pixel].x <= 0.0F || mean < 1.0) {
            continue;
        }
        const f64 variance = std::max(0.0, (squares[pixel] / kFrames) - (mean * mean));
        const f64 raw_mean = raw_sum[pixel] / kFrames;
        const f64 raw_variance =
            std::max(0.0, (raw_squares[pixel] / kFrames) - (raw_mean * raw_mean));
        relative_deviation += std::sqrt(variance) / mean;
        raw_relative_deviation += raw_mean > 0.0 ? std::sqrt(raw_variance) / raw_mean : 0.0;
        ++counted;
    }
    relative_deviation /= static_cast<f64>(std::max(counted, 1U));
    raw_relative_deviation /= static_cast<f64>(std::max(counted, 1U));
    const f64 frame_change = level > 0.0 ? change / level : 0.0;
    std::fprintf(stderr,
                 "(g) %u lit pixels, %u frames after 30: denoised relative deviation %.4f (raw "
                 "%.4f); mean frame-to-frame change %.4f of the signal\n",
                 counted, kFrames, relative_deviation, raw_relative_deviation, frame_change);
    CY_CHECK_GT(counted, 20000U);
    CY_CHECK_LT(relative_deviation, 0.05);
    CY_CHECK_LT(frame_change, 0.05);
    CY_CHECK_LT(relative_deviation * 4.0, raw_relative_deviation);
    stop(run);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

namespace {

/// The mean luminance of the open floor's pixels, and of each pixel.
f64 floor_luminance(const LightScene& run, std::vector<f64>& per_pixel) {
    per_pixel.assign(usize{run.scene->width()} * run.scene->height(), -1.0);
    f64 total = 0.0;
    u32 count = 0;
    for (u32 y = 0; y < run.scene->height(); ++y) {
        for (u32 x = 0; x < run.scene->width(); ++x) {
            Vec3 relative;
            if (!floor_point(run, x, y, relative)) {
                continue;
            }
            const f64 value = luminance(hdr_at(run, x, y));
            per_pixel[(static_cast<usize>(y) * run.scene->width()) + x] = value;
            total += value;
            ++count;
        }
    }
    return count > 0 ? total / count : 0.0;
}

}  // namespace

CY_TEST_CASE(
    "(h) the same content under either path: the two pictures agree where nothing shadows") {
    DeviceFixture fixture("cy_test_render_many_lights");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    const std::vector<render::LightDescription> lights = coloured_lights(160, 8, 2.5F, 900.0F);
    Frame clustered_frame;
    clustered_frame.run.floor_albedo = Vec3{0.5F, 0.5F, 0.5F};
    CY_REQUIRE(build(fixture, clustered_frame, lights, false, kWidth, kHeight));
    CY_REQUIRE(render_frame(clustered_frame.run));
    std::vector<f64> exact;
    const f64 clustered_mean = floor_luminance(clustered_frame.run, exact);

    Stochastic run;
    CY_REQUIRE(start(fixture, run, lights, false, settings_for(true, true, true)));
    for (u32 frame = 0; frame < 40; ++frame) {
        CY_REQUIRE(step(run));
    }
    std::vector<f64> sampled;
    const f64 stochastic_mean = floor_luminance(run.frame.run, sampled);
    f64 difference = 0.0;
    u32 count = 0;
    for (usize pixel = 0; pixel < exact.size(); ++pixel) {
        if (exact[pixel] < 0.0 || sampled[pixel] < 0.0) {
            continue;
        }
        difference += std::fabs(sampled[pixel] - exact[pixel]);
        ++count;
    }
    const f64 mean_difference = count > 0 ? difference / count : 0.0;
    std::fprintf(stderr,
                 "(h) open floor, 160 lights: clustered mean %.3f, stochastic mean %.3f "
                 "(%+.2f%%); mean absolute difference %.2f%% of the clustered mean\n",
                 clustered_mean, stochastic_mean,
                 100.0 * ((stochastic_mean / clustered_mean) - 1.0),
                 100.0 * mean_difference / clustered_mean);
    // THE BOUNDS ARE THE PATH'S CHARACTER, measured: the denoiser's value edge-stopping takes
    // about four per cent of the light (a bright, rare sample is a value edge to it), and a pixel
    // differs from the clustered one by its residual noise and the specular half, which is one
    // reservoir sample and not denoised.
    CY_CHECK_LT(std::fabs((stochastic_mean / clustered_mean) - 1.0), 0.05);
    CY_CHECK_LT(mean_difference / clustered_mean, 0.20);
    // The two paths report the bound that applies: a per-pixel sample count and ray budget.
    CY_CHECK_EQ(run.pass.report().stats.path, rendering::LightingPath::StochasticManyLight);
    CY_CHECK_EQ(run.pass.report().stats.max_lights_per_cluster, 0U);
    CY_CHECK_EQ(run.pass.report().stats.candidates_per_pixel, 16U);
    CY_CHECK_EQ(run.pass.report().stats.visibility_rays_per_pixel, 1U);
    release_scene(clustered_frame.run);
    stop(run);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(i) an area light's shadow is soft: dark under the box, a penumbra around it") {
    DeviceFixture fixture("cy_test_render_many_lights");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // One large panel straight over the scene's boxes.
    render::LightDescription panel;
    panel.kind = render::LightKind::Rect;
    panel.transform.translation = Vec3{0.0F, 1.6F, -6.4F};
    panel.transform.rotation = Quat::from_axis_angle(Vec3{1.0F, 0.0F, 0.0F}, -0.5F * math::kPi);
    panel.source_width = 2.0F;
    panel.source_height = 2.0F;
    panel.intensity = 30000.0F;
    panel.range = 20.0F;
    panel.stable_id = 4242;
    const std::vector<render::LightDescription> lights{panel};
    Frame unshadowed;
    unshadowed.run.floor_albedo = Vec3{0.5F, 0.5F, 0.5F};
    CY_REQUIRE(build(fixture, unshadowed, lights, true, kWidth, kHeight));
    CY_REQUIRE(render_frame(unshadowed.run));
    std::vector<f64> open;
    (void)floor_luminance(unshadowed.run, open);

    Stochastic run;
    CY_REQUIRE(start(fixture, run, lights, true, settings_for(true, true, true)));
    for (u32 frame = 0; frame < 40; ++frame) {
        CY_REQUIRE(step(run));
    }
    std::vector<f64> shadowed;
    (void)floor_luminance(run.frame.run, shadowed);
    u32 umbra = 0;
    u32 penumbra = 0;
    u32 lit = 0;
    for (usize pixel = 0; pixel < open.size(); ++pixel) {
        if (open[pixel] <= 1.0 || shadowed[pixel] < 0.0) {
            continue;
        }
        const f64 ratio = shadowed[pixel] / open[pixel];
        umbra += ratio < 0.35 ? 1U : 0U;
        penumbra += ratio >= 0.45 && ratio <= 0.85 ? 1U : 0U;
        lit += ratio > 0.9 ? 1U : 0U;
    }
    std::fprintf(stderr,
                 "(i) floor under a 2 m panel: %u pixels in umbra, %u in penumbra, %u lit\n", umbra,
                 penumbra, lit);
    save("many-lights-soft-shadow.png", run.frame.scene);
    CY_CHECK_GT(umbra, 200U);
    CY_CHECK_GT(penumbra, 400U);
    CY_CHECK_GT(lit, 5000U);
    release_scene(unshadowed.run);
    stop(run);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(j) hundreds of coloured lights, shadowed, on the stochastic path") {
    DeviceFixture fixture("cy_test_render_many_lights");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Stochastic run;
    CY_REQUIRE(start(fixture, run, coloured_lights(400, 9, 2.2F, 1500.0F), true,
                     settings_for(true, true, true)));
    for (u32 frame = 0; frame < 30; ++frame) {
        CY_REQUIRE(step(run));
    }
    save("many-lights-stochastic.png", run.frame.scene);
    CY_CHECK_GT(run.pass.report().candidate_lights, 390U);
    stop(run);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

namespace {

/// One clustered frame's inputs to the path selection: the device assignment's drops and the
/// measured lighting cost, prepass end to opaque end, at its steady state.
struct ClusteredCost {
    u32 dropped = 0;
    f32 lighting_ms = 0.0F;
};

[[nodiscard]] bool clustered_cost(DeviceFixture& fixture,
                                  std::vector<render::LightDescription> lights,
                                  ClusteredCost& out) {
    GpuLightAssignment assignment;
    Frame frame;
    frame.run.fixture = &fixture;
    frame.run.lights = std::move(lights);
    frame.run.boxes = false;
    frame.run.floor_albedo = Vec3{0.5F, 0.5F, 0.5F};
    frame.run.assignment = &assignment;
    const u32 capacity = static_cast<u32>(frame.run.lights.size()) + 4U;
    if (!build_scene(frame.run, frame.scene, kWidth, kHeight, capacity)) {
        return false;
    }
    const rendering::assembly::AssemblyDescription& assembled =
        frame.scene.assembly().description();
    Expected<rendering::ClusterGrid, Error> grid = rendering::make_cluster_grid(
        assembled.clusters, kWidth, kHeight, assembled.near_plane, assembled.far_plane);
    if (!grid.has_value() || !assignment
                                  .create(allocator(), fixture.device(),
                                          LightAssignmentDescription{*grid, 4096, 65536, false})
                                  .has_value()) {
        return false;
    }
    f64 lighting_ms = 1.0e9;
    for (u32 round = 0; round < 4; ++round) {
        if (!render_frame(frame.run)) {
            return false;
        }
        lighting_ms = std::min(
            lighting_ms, frame.run.stamps_ms[kStampOpaque] - frame.run.stamps_ms[kStampPrepass]);
    }
    out.dropped = assignment.statistics().overflow;
    out.lighting_ms = static_cast<f32>(lighting_ms);
    release_scene(frame.run);
    assignment.destroy();
    return true;
}

}  // namespace

CY_TEST_CASE(
    "(k) the frame arbiter's choice: past the cluster bound the frame leaves, and returns") {
    DeviceFixture fixture("cy_test_render_many_lights");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    rendering::BudgetArbiter arbiter;
    CY_REQUIRE(rendering::apply_profile(rendering::named_profile(rendering::ProfileName::Standard),
                                        arbiter)
                   .has_value());
    const f32 allocation = rendering::shading_allocation_ms(arbiter);
    const ManyLightsSettings settings = settings_for(true, true, true);
    rendering::LightingPathSelector selector(8, 0.7F);

    // Six hundred lights crowded over four square metres: far more reach each cluster than its
    // list of 64 keeps, so the clustered picture is missing light it should have.
    constexpr u32 kCrowd = 600;
    ClusteredCost crowded;
    CY_REQUIRE(clustered_cost(
        fixture, coloured_lights(kCrowd, 0, 3.0F, 900.0F, Spread{Vec3{0.0F, -1.9F, -8.0F}, 2.0F}),
        crowded));
    rendering::LightingPathInputs inputs;
    inputs.local_lights = kCrowd;
    inputs.lights_dropped_by_bound = crowded.dropped;
    inputs.clustered_ms = crowded.lighting_ms;
    inputs.allocation_ms = allocation;
    const rendering::LightingPathDecision left = selector.update(settings.path, inputs);
    std::fprintf(
        stderr,
        "(k) %u crowded lights: %u assignments dropped at the bound, clustered lighting "
        "%.3f ms against the arbiter's %.3f ms: %s (%s)\n",
        kCrowd, crowded.dropped, static_cast<f64>(crowded.lighting_ms),
        static_cast<f64>(allocation),
        left.path == rendering::LightingPath::StochasticManyLight ? "stochastic" : "clustered",
        left.reason);
    CY_CHECK_GT(crowded.dropped, 0U);
    CY_CHECK_EQ(left.path, rendering::LightingPath::StochasticManyLight);
    CY_CHECK(left.changed);

    // The frame the decision draws: the stochastic path, its bound a per-pixel sample count.
    Stochastic run;
    CY_REQUIRE(
        start(fixture, run,
              coloured_lights(kCrowd, 0, 3.0F, 900.0F, Spread{Vec3{0.0F, -1.9F, -8.0F}, 2.0F}),
              false, settings));
    for (u32 frame = 0; frame < 4; ++frame) {
        CY_REQUIRE(step(run));
    }
    CY_CHECK_EQ(run.pass.report().stats.path, rendering::LightingPath::StochasticManyLight);
    CY_CHECK_EQ(run.pass.report().candidate_lights, kCrowd);
    stop(run);

    // A hundred lights spread wide: the clustered lighting fits the allocation with no drop, so
    // the frame returns — but only after the dwell, never on the first quiet frame.
    constexpr u32 kSparse = 100;
    ClusteredCost sparse;
    CY_REQUIRE(clustered_cost(fixture, coloured_lights(kSparse, 0, 1.6F, 900.0F), sparse));
    CY_CHECK_EQ(sparse.dropped, 0U);
    CY_CHECK_LT(sparse.lighting_ms, allocation);
    rendering::LightingPathInputs calm;
    calm.local_lights = kSparse;
    calm.allocation_ms = allocation;
    for (u32 frame = 0; frame < 7; ++frame) {
        CY_CHECK_EQ(selector.update(settings.path, calm).path,
                    rendering::LightingPath::StochasticManyLight);
    }
    const rendering::LightingPathDecision returned = selector.update(settings.path, calm);
    CY_CHECK_EQ(returned.path, rendering::LightingPath::Clustered);
    CY_CHECK(returned.changed);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(l) past its reach a light adds exactly zero: the clusters draw the walk's frame") {
    DeviceFixture fixture("cy_test_render_many_lights");
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // Forty lights at night, few enough that no cluster's list is full, with points, spots and
    // every shape of area light: each pixel sees lights whose reach ends just short of it.
    const std::vector<render::LightDescription> lights = coloured_lights(40, 5, 2.0F, 900.0F);
    Frame clustered;
    clustered.run.floor_albedo = Vec3{0.5F, 0.5F, 0.5F};
    clustered.run.night = true;
    CY_REQUIRE(build(fixture, clustered, lights, true, kWidth, kHeight));
    CY_REQUIRE(render_frame(clustered.run));
    Frame walked;
    walked.run.floor_albedo = Vec3{0.5F, 0.5F, 0.5F};
    walked.run.night = true;
    walked.run.walk_every_light = true;
    CY_REQUIRE(build(fixture, walked, lights, true, kWidth, kHeight));
    CY_REQUIRE(render_frame(walked.run));
    CY_REQUIRE_EQ(clustered.run.hdr.size(), walked.run.hdr.size());
    u32 differing = 0;
    for (usize index = 0; index < clustered.run.hdr.size(); ++index) {
        differing += clustered.run.hdr[index] != walked.run.hdr[index] ? 1U : 0U;
    }
    u32 lit = 0;
    for (usize index = 0; index < walked.run.hdr.size(); index += 4) {
        lit += walked.run.hdr[index] != 0U ? 1U : 0U;
    }
    std::fprintf(stderr,
                 "(l) %zu lights: %u HDR halves differ between the cluster loop and the walk over "
                 "every light (%u lit pixels); cluster overflow %u\n",
                 lights.size(), differing, lit, clustered.run.report.clusters.overflow);
    CY_CHECK_EQ(clustered.run.report.clusters.overflow, 0U);
    CY_CHECK_GT(lit, 1000U);
    CY_CHECK_EQ(differing, 0U);
    release_scene(clustered.run);
    release_scene(walked.run);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
