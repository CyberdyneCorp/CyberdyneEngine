// SPDX-License-Identifier: MIT
// CyberGI on a device, against the host implementation it transcribes. Issue #35, stages 1 and 2.
//
// ================================================================================================
// WHAT THIS SUITE ASSERTS
// ================================================================================================
//
// Every dispatch in cy::rendering-gi-gpu is a transcription of a host function in cy::rendering-gi,
// and every case here runs both over one scene and compares BUFFERS:
//
//   the field      the uploaded clipmap, sphere traced on the device ray for ray, against
//                  `DistanceField::sphere_trace` over the same rays;
//   incremental    a moved object re-uploads the bricks the field re-solved and nothing else, and a
//                  still frame uploads nothing;
//   card radiance  the device-shaded surface cache against the host one, page for page, frame after
//                  frame — direct light (the shadow map for the sun, the field for a point light),
//                  the sky term and last frame's cards as the bounce;
//   budget         a budgeted update shades exactly the pages `SurfaceCache::select` chose and
//                  leaves every other page untouched.
//
// The host side of each comparison is the oracle, `gi::CardGather` over a `gi::CardSnapshot` taken
// before the update and `gi::ShadowMapOccluder` over `gi::SoftwareTracer` — the host functions the
// shaders transcribe, not a second implementation written for the test.
//
// ================================================================================================
// FLOATING POINT
// ================================================================================================
//
// A driver may contract a multiply and an add into one fused operation where the host does not,
// and a sphere trace amplifies a last-bit difference into a different step count only where a
// march grazes a surface. So the bounds below are on the FRACTION of answers that differ beyond a
// small tolerance and on the size of the typical difference, with the measured numbers printed and
// stated beside each bound — the shape `render.skinning` uses.
//
// ================================================================================================
// THE MUTATIONS THIS SUITE IS PROVED AGAINST
// ================================================================================================
//
// Each applied to the tree, run on the RTX 5060, restored and md5-verified; the full record, with
// every case each one turns red, is openspec/changes/add-gpu-gi-surface-cache/evidence/
// falsification.txt:
//
//   gi_gpu_common.slang giTrilinear: `tz` -> `ty`                   the trace case, and four more.
//   gpu_scene.cpp upload_field: always `full`                        the incremental case.
//   gi_cards.slang: `accumulated = albedo * incoming` -> `incoming`  both radiance cases.
//   gi_gpu_common.slang giOccluded: the shadow-map branch removed    the shadow-map case.
//   surface_cache.cpp select(): the budget limit dropped             the budget case.
//   gi_gpu_common.slang giGather: the origin not lifted off the card both radiance cases.

#include <cy/backends/rhi/device.h>
#include <cy/core/math/matrix.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi/card_lighting.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/surface_cache.h>
#include <cy/rendering/gi/tracing.h>
#include <cy/rendering/gi_gpu/gpu_scene.h>
#include <cy/rendering/gi_gpu/gpu_surface_cache.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/test/test.h>

#include "device.h"
#include "golden.h"
#include "support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace cy;
using cy::rendering::gi::CardGather;
using cy::rendering::gi::CardGatherSettings;
using cy::rendering::gi::CardSnapshot;
using cy::rendering::gi::ClipmapSettings;
using cy::rendering::gi::DistanceField;
using cy::rendering::gi::GiLight;
using cy::rendering::gi::ShadowMap;
using cy::rendering::gi::ShadowMapOccluder;
using cy::rendering::gi::ShadowMapSettings;
using cy::rendering::gi::SoftwareTracer;
using cy::rendering::gi::SurfaceCache;
using cy::rendering::gi::SurfacePage;
using cy::rendering::gi::SurfaceUpdateContext;
using cy::rendering::gi::Surfel;
using cy::rendering::gi_gpu::GpuGiScene;
using cy::rendering::gi_gpu::GpuGiSceneDescription;
using cy::rendering::gi_gpu::GpuSurfaceShading;
using cy::rendering::gi_gpu::GpuTraceHit;
using cy::rendering::gi_gpu::GpuTraceRay;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

/// A device, or a loud skip. The message is the one the suite's SKIP_REGULAR_EXPRESSION matches.
struct Gpu {
    render_test::DeviceFixture fixture{"vulkan", "render.gi_gpu"};

    [[nodiscard]] bool available() const noexcept {
        if (fixture.is(rhi::BackendKind::Vulkan) && GpuGiScene::supported(fixture.device())) {
            return true;
        }
        std::fprintf(stderr, "no requested graphics device on this machine; ");
        fixture.report_skip();
        return false;
    }
};

/// One graph, executed and drained. `declare` adds the passes.
template <class Declare>
[[nodiscard]] bool run_frame(rhi::Device& device, Declare&& declare) {
    auto begun = device.begin_frame();
    if (!begun.has_value()) {
        std::fprintf(stderr, "begin_frame failed: %s\n", begun.error().message);
        return false;
    }
    rendering::RenderGraph graph(allocator());
    if (Status declared = declare(graph); !declared) {
        std::fprintf(stderr, "declare failed: %s\n", declared.error().message);
        return false;
    }
    rendering::GraphExecutor executor(allocator(), device);
    auto executed =
        executor.execute(graph, rendering::CompileOptions{}, rendering::ExecuteOptions{});
    if (!executed.has_value()) {
        std::fprintf(stderr, "execute failed: %s\n", executed.error().message);
        return false;
    }
    return device.wait_idle().has_value() && device.end_frame().has_value();
}

// --- Scenes --------------------------------------------------------------------------------------

/// The host GI suites' room, in its field: one level, a quarter-metre voxel, a 16-brick window.
struct RoomScene {
    gi_support::RoomField room{Vec3{gi_support::kRoomX, gi_support::kRoomY, gi_support::kRoomZ}};
    DistanceField field;

    RoomScene() {
        const ClipmapSettings settings = gi_support::room_settings().field;
        CY_REQUIRE(field.configure(settings).has_value());
        CY_REQUIRE(field.place(1, room.asset(), Mat4::identity()).has_value());
        (void)field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});
    }
};

/// An open courtyard under a sun: a floor slab, a pillar that casts a shadow across it, and a red
/// wall behind whose colour the floor picks up through the bounce.
struct CourtyardScene {
    gi_support::BoxField slab{Vec3{6.0F, 0.5F, 6.0F}, 1.0F, 17};
    gi_support::BoxField pillar{Vec3{0.5F, 1.5F, 0.5F}, 1.0F, 9};
    gi_support::BoxField wall{Vec3{4.0F, 1.5F, 0.25F}, 1.0F, 13};
    DistanceField field;
    std::vector<Surfel> surfels;
    ShadowMap shadow;
    GiLight sun;
    GiLight lamp;

    static constexpr f32 kFloorY = 0.0F;

    CourtyardScene() {
        ClipmapSettings settings;
        settings.levels = 1;
        settings.resolution = 64;
        settings.base_extent_metres = 16.0F;
        CY_REQUIRE(field.configure(settings).has_value());
        CY_REQUIRE(field.place(1, slab.asset(), Mat4::from_translation(Vec3{0.0F, -0.5F, 0.0F}))
                       .has_value());
        CY_REQUIRE(field.place(2, pillar.asset(), Mat4::from_translation(Vec3{0.0F, 1.5F, 0.0F}))
                       .has_value());
        CY_REQUIRE(field.place(3, wall.asset(), Mat4::from_translation(Vec3{0.0F, 1.5F, -3.0F}))
                       .has_value());
        (void)field.scroll_to(Vec3{0.0F, 1.0F, 0.0F});

        const auto add = [this](Vec3 position, Vec3 normal, Vec3 albedo, f32 area) {
            Surfel surfel;
            surfel.position = position;
            surfel.normal = normal;
            surfel.albedo = albedo;
            surfel.area = area;
            surfels.push_back(surfel);
        };
        // The floor, on a half-metre lattice, minus what the pillar and the wall stand on.
        for (i32 iz = -10; iz <= 10; ++iz) {
            for (i32 ix = -10; ix <= 10; ++ix) {
                const Vec3 p{static_cast<f32>(ix) * 0.5F, kFloorY, static_cast<f32>(iz) * 0.5F};
                if ((std::abs(p.x) < 0.5F && std::abs(p.z) < 0.5F) ||
                    (std::abs(p.x) < 4.0F && std::abs(p.z + 3.0F) < 0.25F)) {
                    continue;
                }
                add(p, Vec3{0.0F, 1.0F, 0.0F}, Vec3{0.7F, 0.7F, 0.7F}, 0.25F);
            }
        }
        // The pillar's four sides and the red wall's face toward the floor.
        for (i32 iy = 0; iy < 6; ++iy) {
            const f32 y = 0.25F + (static_cast<f32>(iy) * 0.5F);
            add(Vec3{0.5F, y, 0.0F}, Vec3{1.0F, 0.0F, 0.0F}, Vec3{0.6F, 0.6F, 0.6F}, 0.5F);
            add(Vec3{-0.5F, y, 0.0F}, Vec3{-1.0F, 0.0F, 0.0F}, Vec3{0.6F, 0.6F, 0.6F}, 0.5F);
            add(Vec3{0.0F, y, 0.5F}, Vec3{0.0F, 0.0F, 1.0F}, Vec3{0.6F, 0.6F, 0.6F}, 0.5F);
            add(Vec3{0.0F, y, -0.5F}, Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.6F, 0.6F, 0.6F}, 0.5F);
            for (i32 ix = -7; ix <= 7; ++ix) {
                add(Vec3{static_cast<f32>(ix) * 0.5F, y, -2.75F}, Vec3{0.0F, 0.0F, 1.0F},
                    Vec3{0.85F, 0.08F, 0.06F}, 0.25F);
            }
        }

        sun.directional = true;
        sun.direction = normalized_or(Vec3{0.45F, -1.0F, 0.35F}, Vec3{0.0F, -1.0F, 0.0F});
        sun.colour = Vec3{1.0F, 0.95F, 0.85F};
        sun.intensity = 3.0F;
        sun.id = 1;
        lamp.position = Vec3{2.5F, 1.2F, 2.5F};
        lamp.colour = Vec3{0.6F, 0.8F, 1.0F};
        lamp.intensity = 4.0F;
        lamp.range = 12.0F;
        lamp.id = 2;

        ShadowMapSettings map;
        map.direction = sun.direction;
        map.centre = Vec3{0.0F, 1.0F, 0.0F};
        map.half_extent_metres = 7.0F;
        map.depth_range_metres = 24.0F;
        map.resolution = 128;
        map.bias_metres = 0.3F;
        CY_REQUIRE(shadow.configure(map).has_value());
        shadow.capture(field);
    }
};

// --- The host oracle and the device cache, side by side ------------------------------------------

/// Two surface caches over the same cards — one shaded on the host through the oracle, one through
/// the device — and the device scene behind the second.
struct Pair {
    static constexpr f32 kLookupRadius = 0.6F;

    SurfaceCache host;
    SurfaceCache device;
    SoftwareTracer software;
    ShadowMapOccluder occluder;
    CardSnapshot snapshot;
    CardGather gather;
    CardGatherSettings gather_settings;
    GpuGiScene scene;
    GpuSurfaceShading shading;
    std::vector<GiLight> lights;

    Pair(rhi::Device& gpu, const DistanceField& field, const std::vector<Surfel>& surfels,
         const ShadowMap* shadow, std::vector<GiLight> scene_lights, u32 rays)
        : lights(std::move(scene_lights)) {
        // NOT THE DEFAULT HALF METRE. Both scenes are dyadic — cards on a half- or one-metre
        // lattice, a quarter-metre voxel, a gather that leaves at two voxels — and at a lookup
        // radius of exactly half a metre over a hundred of the room's gather hits land ON the
        // lookup sphere of a card: a ceiling card's ray with direction z = 0.75 meets the wall at
        // squared distance 0.25 from the wall card below it. Whether such a hit resolves is then
        // the last bit of a sine, and the host and a driver round it differently (measured on the
        // RTX 5060: 0.25000003 on the host, 0.24999997 on the device, flipping a third of the
        // room's bounce terms). A radius off the lattice keeps this comparison about the
        // transcription rather than about a tie.
        host.set_lookup_radius(kLookupRadius);
        device.set_lookup_radius(kLookupRadius);
        for (const Surfel& surfel : surfels) {
            CY_REQUIRE(host.allocate(surfel).has_value());
            CY_REQUIRE(device.allocate(surfel).has_value());
        }
        software.bind(field, nullptr);
        occluder.bind(shadow, &software);
        gather_settings.rays = rays;
        gather_settings.max_distance_metres = 24.0F;
        gather.bind(&field, &snapshot, gather_settings);

        GpuGiSceneDescription description;
        description.max_levels = 2;
        description.max_window_bricks = 16;
        description.max_brick_slots = 16384;
        description.max_cards = 2048;
        description.max_selection = 2048;
        CY_REQUIRE(scene.create(allocator(), gpu, description).has_value());
        shading.bind(scene, field, device.lookup_radius());
        shading.set_shadow_map(shadow);
        shading.set_gather(gather_settings);
        device.set_shading_backend(&shading);
    }

    [[nodiscard]] SurfaceUpdateContext host_context(u64 frame, u32 budget) const noexcept {
        SurfaceUpdateContext context;
        context.lights = {lights.data(), lights.size()};
        context.occluder = &occluder;
        context.indirect = &gather;
        context.budget = budget;
        context.frame = frame;
        return context;
    }

    [[nodiscard]] SurfaceUpdateContext device_context(u64 frame, u32 budget) const noexcept {
        SurfaceUpdateContext context;
        context.lights = {lights.data(), lights.size()};
        context.budget = budget;
        context.frame = frame;
        return context;
    }

    /// One frame on both sides: the snapshot, the host update, the device update and its dispatch.
    [[nodiscard]] bool step(rhi::Device& gpu, u64 frame, u32 budget, bool all) {
        CY_REQUIRE(snapshot.capture(host, frame).has_value());
        (void)(all ? host.update_all(host_context(frame, budget))
                   : host.update(host_context(frame, budget)));
        const auto report = all ? device.update_all(device_context(frame, budget))
                                : device.update(device_context(frame, budget));
        if (report.pages_updated == 0) {
            std::fprintf(stderr, "the device accepted no pages: %s\n",
                         shading.last_error().message);
            return false;
        }
        if (!run_frame(gpu,
                       [&](rendering::RenderGraph& graph) { return shading.declare(graph); })) {
            return false;
        }
        return device.collect() == report.pages_updated;
    }
};

struct Agreement {
    u32 compared = 0;
    /// Pages whose relative difference exceeds the case's tolerance.
    u32 beyond = 0;
    f64 mean_relative = 0.0;
    f64 worst_relative = 0.0;
};

[[nodiscard]] f64 relative(Vec3 a, Vec3 b) noexcept {
    const f64 scale = std::max({static_cast<f64>(length(a)), static_cast<f64>(length(b)), 1.0e-3});
    return static_cast<f64>(length(a - b)) / scale;
}

template <class Term>
[[nodiscard]] Agreement agree(const SurfaceCache& host, const SurfaceCache& device, f64 tolerance,
                              Term&& term) {
    Agreement result;
    f64 total = 0.0;
    for (usize handle = 0; handle < host.pages().size(); ++handle) {
        const SurfacePage& a = host.pages()[handle];
        const SurfacePage& b = device.pages()[handle];
        if (!a.live) {
            continue;
        }
        const f64 difference = relative(term(a), term(b));
        total += difference;
        result.worst_relative = std::max(result.worst_relative, difference);
        result.beyond += difference > tolerance ? 1U : 0U;
        result.compared += 1;
    }
    result.mean_relative = result.compared == 0 ? 0.0 : total / result.compared;
    return result;
}

void print(const char* what, const Agreement& agreement) {
    std::fprintf(stderr, "%s: %u pages, mean relative %.3g, worst %.3g, %u beyond tolerance\n",
                 what, agreement.compared, agreement.mean_relative, agreement.worst_relative,
                 agreement.beyond);
}

/// Rays in every direction from a handful of points inside the room.
[[nodiscard]] std::vector<GpuTraceRay> room_rays() {
    std::vector<GpuTraceRay> rays;
    const Vec3 origins[] = {{0.0F, 0.0F, 2.0F},   {-2.5F, 1.0F, -2.0F}, {2.0F, -1.2F, -1.0F},
                            {3.1F, 0.4F, 3.3F},   {-0.3F, 1.6F, 0.9F},  {1.7F, -0.5F, -3.2F},
                            {-3.4F, -1.5F, 3.0F}, {0.6F, 0.1F, -0.6F}};
    constexpr u32 kPerOrigin = 128;
    for (const Vec3 origin : origins) {
        for (u32 index = 0; index < kPerOrigin; ++index) {
            // The golden-ratio sphere: every direction, none repeated.
            const f32 y = 1.0F - (2.0F * (static_cast<f32>(index) + 0.5F) / kPerOrigin);
            const f32 radius = std::sqrt(std::max(0.0F, 1.0F - (y * y)));
            const f32 angle = 2.39996323F * static_cast<f32>(index);
            GpuTraceRay ray;
            ray.origin = origin;
            ray.direction = Vec3{radius * std::cos(angle), y, radius * std::sin(angle)};
            ray.max_distance = 24.0F;
            ray.t_min = (index % 3U) == 0U ? 0.2F : 0.0F;
            rays.push_back(ray);
        }
    }
    return rays;
}

struct TraceAgreement {
    u32 rays = 0;
    u32 hit_disagreements = 0;
    u32 hits = 0;
    u32 t_beyond = 0;
    f32 worst_t = 0.0F;
    f32 worst_normal = 0.0F;
};

[[nodiscard]] TraceAgreement trace_both(rhi::Device& gpu, GpuGiScene& scene,
                                        const DistanceField& field,
                                        const std::vector<GpuTraceRay>& rays) {
    TraceAgreement result;
    CY_REQUIRE(scene.set_rays({rays.data(), rays.size()}).has_value());
    CY_REQUIRE(
        run_frame(gpu, [&](rendering::RenderGraph& graph) { return scene.declare_trace(graph); }));
    std::vector<GpuTraceHit> hits(rays.size());
    CY_REQUIRE(scene.read_back_hits({hits.data(), hits.size()}).has_value());
    for (usize index = 0; index < rays.size(); ++index) {
        Ray ray;
        ray.origin = rays[index].origin;
        ray.direction = rays[index].direction;
        const auto expected = field.sphere_trace(ray, rays[index].max_distance, rays[index].t_min);
        const GpuTraceHit& measured = hits[index];
        result.rays += 1;
        if (expected.hit != measured.hit) {
            result.hit_disagreements += 1;
            continue;
        }
        if (!expected.hit) {
            continue;
        }
        result.hits += 1;
        const f32 dt = std::abs(expected.t - measured.t);
        result.worst_t = std::max(result.worst_t, dt);
        result.t_beyond += dt > 1.0e-3F ? 1U : 0U;
        result.worst_normal =
            std::max(result.worst_normal, 1.0F - dot(expected.normal, measured.normal));
    }
    std::fprintf(stderr,
                 "trace: %u rays, %u hits, %u hit/miss disagreements, %u hits with |dt| > 1 mm, "
                 "worst |dt| %.3g m, worst 1 - n.n %.3g\n",
                 result.rays, result.hits, result.hit_disagreements, result.t_beyond,
                 static_cast<double>(result.worst_t), static_cast<double>(result.worst_normal));
    return result;
}

}  // namespace

// --- Stage 1 -------------------------------------------------------------------------------------

CY_TEST_CASE("the uploaded field traces as the host field does") {
    Gpu gpu;
    if (!gpu.available()) {
        return;
    }
    RoomScene room;
    GpuGiScene scene;
    CY_REQUIRE(
        scene.create(allocator(), gpu.fixture.device(), GpuGiSceneDescription{}).has_value());
    const auto uploaded = scene.upload_field(room.field);
    CY_REQUIRE(uploaded.has_value());
    CY_CHECK(uploaded->full);
    CY_CHECK_EQ(uploaded->bricks_uploaded, room.field.diagnostics().allocated_bricks);

    const TraceAgreement agreement =
        trace_both(gpu.fixture.device(), scene, room.field, room_rays());
    // A closed room: every ray hits a wall, so a dispatch that never ran — every answer a miss —
    // disagrees on all of them rather than agreeing by absence.
    CY_CHECK_GT(agreement.hits, agreement.rays * 9U / 10U);
    // MEASURED on the RTX 5060: 0 disagreements, 0 hits beyond a millimetre, worst |dt| 9.5e-7 m.
    CY_CHECK_LE(agreement.hit_disagreements, agreement.rays / 100U);
    CY_CHECK_LE(agreement.t_beyond, agreement.hits / 100U);
    CY_CHECK_LT(agreement.worst_normal, 0.02F);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("a moved object re-uploads only the bricks it touched") {
    Gpu gpu;
    if (!gpu.available()) {
        return;
    }
    const gi_support::BoxField floor(Vec3{4.0F, 0.25F, 4.0F}, 0.5F, 17);
    const gi_support::BoxField door(Vec3{0.5F, 1.0F, 0.1F}, 0.3F, 9);
    DistanceField field;
    ClipmapSettings settings;
    settings.levels = 2;
    settings.resolution = 32;
    settings.base_extent_metres = 8.0F;
    CY_REQUIRE(field.configure(settings).has_value());
    CY_REQUIRE(
        field.place(1, floor.asset(), Mat4::from_translation(Vec3{0.0F, -1.5F, 0.0F})).has_value());
    CY_REQUIRE(field.place(7, door.asset(), Mat4::identity()).has_value());
    (void)field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});

    GpuGiScene scene;
    CY_REQUIRE(
        scene.create(allocator(), gpu.fixture.device(), GpuGiSceneDescription{}).has_value());
    const auto first = scene.upload_field(field);
    CY_REQUIRE(first.has_value());
    CY_CHECK(first->full);
    const u32 window = field.diagnostics().last_scroll.bricks_solved;

    // A still frame: the field scrolls to where it already is and solves nothing, and the device
    // copy is one generation behind and receives nothing.
    (void)field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});
    const auto still = scene.upload_field(field);
    CY_REQUIRE(still.has_value());
    CY_CHECK_FALSE(still->full);
    CY_CHECK_EQ(still->table_entries, 0U);
    CY_CHECK_EQ(still->bricks_uploaded, 0U);

    // The door swings. The bricks it left and the bricks it entered are the only ones re-solved,
    // and they are the only ones the device receives.
    const Aabb before = cy::transformed(door.bounds, Mat4::identity());
    const Mat4 opened = Mat4::from_translation(Vec3{2.5F, 0.0F, 0.0F});
    CY_REQUIRE(field.move(7, opened).has_value());
    const auto scrolled = field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});
    const auto moved = scene.upload_field(field);
    CY_REQUIRE(moved.has_value());
    CY_CHECK_FALSE(moved->full);
    CY_CHECK_GT(moved->table_entries, 0U);
    CY_CHECK_EQ(moved->table_entries, scrolled.bricks_solved);
    CY_CHECK_LT(moved->table_entries, window / 4U);
    std::fprintf(
        stderr,
        "door moved: %u of %u bricks re-solved and uploaded (%u with samples, %llu bytes)\n",
        moved->table_entries, window, moved->bricks_uploaded,
        static_cast<unsigned long long>(moved->bytes));

    // Every uploaded brick overlaps where the door was or where it is — the invalidation's own
    // one-brick margin included — and none is anywhere else.
    const Aabb after = cy::transformed(door.bounds, opened);
    u32 outside = 0;
    for (const auto& change : field.last_changes()) {
        const auto view = field.level_view(change.level);
        const Vec3 lo{static_cast<f32>(change.brick[0]) * view.brick_size,
                      static_cast<f32>(change.brick[1]) * view.brick_size,
                      static_cast<f32>(change.brick[2]) * view.brick_size};
        const Aabb brick{lo, lo + Vec3{view.brick_size, view.brick_size, view.brick_size}};
        const f32 margin = view.brick_size * 1.01F;
        if (!brick.intersects(before.expanded(margin)) &&
            !brick.intersects(after.expanded(margin))) {
            outside += 1;
        }
    }
    CY_CHECK_EQ(outside, 0U);

    // And the device copy followed it: rays across the doorway agree with the host field now.
    std::vector<GpuTraceRay> rays;
    for (i32 index = -20; index <= 20; ++index) {
        GpuTraceRay ray;
        ray.origin = Vec3{static_cast<f32>(index) * 0.2F, 0.0F, 3.0F};
        ray.direction = Vec3{0.0F, 0.0F, -1.0F};
        ray.max_distance = 6.0F;
        rays.push_back(ray);
    }
    const TraceAgreement agreement = trace_both(gpu.fixture.device(), scene, field, rays);
    CY_CHECK_GT(agreement.hits, 0U);
    CY_CHECK_EQ(agreement.hit_disagreements, 0U);
    CY_CHECK_EQ(agreement.t_beyond, 0U);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

// --- Stage 2 -------------------------------------------------------------------------------------

CY_TEST_CASE("the device card radiance matches the host surface cache") {
    Gpu gpu;
    if (!gpu.available()) {
        return;
    }
    RoomScene room;
    const std::vector<Surfel> surfels = gi_support::room_surfels(1.0F);
    Pair pair(gpu.fixture.device(), room.field, surfels, nullptr, gi_support::room_lights(), 8);

    // Six frames of the converged update: each adds a bounce, so the last compares the multi-bounce
    // solution rather than one pass of direct light.
    for (u64 frame = 1; frame <= 6; ++frame) {
        CY_REQUIRE(pair.step(gpu.fixture.device(), frame, 0, true));
        const Agreement direct = agree(pair.host, pair.device, 1.0e-3,
                                       [](const SurfacePage& page) { return page.direct; });
        const Agreement bounce = agree(pair.host, pair.device, 1.0e-2,
                                       [](const SurfacePage& page) { return page.accumulated; });
        std::fprintf(stderr, "frame %llu\n", static_cast<unsigned long long>(frame));
        print("  direct", direct);
        print("  accumulated", bounce);
        // MEASURED on the RTX 5060: direct mean 3.6e-8, the bounce mean 3.7e-8 at frame six, no
        // page beyond either tolerance. The bounds leave room for a driver that contracts
        // differently; a transcription error lands orders of magnitude above them.
        CY_CHECK_LE(direct.beyond, direct.compared / 100U);
        CY_CHECK_LT(direct.mean_relative, 1.0e-3);
        CY_CHECK_LE(bounce.beyond, bounce.compared / 20U);
        CY_CHECK_LT(bounce.mean_relative, 1.0e-2);
    }

    // The scene is not trivial: the light reaches some pages and the divider shadows others, and
    // the bounce lights pages the light never reaches.
    u32 lit = 0;
    u32 dark = 0;
    u32 bounced_only = 0;
    for (const SurfacePage& page : pair.device.pages()) {
        const bool direct = length(page.direct) > 1.0e-4F;
        lit += direct ? 1U : 0U;
        dark += direct ? 0U : 1U;
        bounced_only += (!direct && length(page.accumulated) > 1.0e-4F) ? 1U : 0U;
    }
    std::fprintf(stderr, "room: %u lit, %u unlit, %u lit by the bounce alone\n", lit, dark,
                 bounced_only);
    CY_CHECK_GT(lit, 0U);
    CY_CHECK_GT(dark, 0U);
    CY_CHECK_GT(bounced_only, 0U);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the shadow map shadows the device cards as it shadows the host ones") {
    Gpu gpu;
    if (!gpu.available()) {
        return;
    }
    CourtyardScene courtyard;
    Pair pair(gpu.fixture.device(), courtyard.field, courtyard.surfels, &courtyard.shadow,
              {courtyard.sun, courtyard.lamp}, 8);
    for (u64 frame = 1; frame <= 3; ++frame) {
        CY_REQUIRE(pair.step(gpu.fixture.device(), frame, 0, true));
    }
    const Agreement direct =
        agree(pair.host, pair.device, 1.0e-3, [](const SurfacePage& page) { return page.direct; });
    const Agreement bounce = agree(pair.host, pair.device, 1.0e-2,
                                   [](const SurfacePage& page) { return page.accumulated; });
    print("courtyard direct", direct);
    print("courtyard accumulated", bounce);
    CY_CHECK_LE(direct.beyond, direct.compared / 100U);
    CY_CHECK_LE(bounce.beyond, bounce.compared / 20U);

    // The map is doing the shadowing: floor cards under the pillar's shadow receive the lamp but
    // not the sun, and the host and the device agree about which ones they are.
    u32 shadowed = 0;
    u32 sunlit = 0;
    for (usize handle = 0; handle < courtyard.surfels.size(); ++handle) {
        const Surfel& surfel = courtyard.surfels[handle];
        if (surfel.normal.y < 0.5F) {
            continue;
        }
        const bool in_shadow =
            courtyard.shadow.shadowed(surfel.position + (surfel.normal * 1.0e-2F));
        const SurfacePage& page = pair.device.pages()[handle];
        const Vec3 sun_only =
            cy::rendering::gi::direct_radiance(courtyard.sun, page.position, page.normal);
        const Vec3 lamp_only =
            cy::rendering::gi::direct_radiance(courtyard.lamp, page.position, page.normal);
        if (in_shadow) {
            shadowed += 1;
            CY_CHECK_LT(page.direct.y, (sun_only.y * 0.5F) + lamp_only.y + 1.0e-3F);
        } else {
            sunlit += 1;
        }
    }
    std::fprintf(stderr, "courtyard floor: %u cards in the pillar's shadow, %u in the sun\n",
                 shadowed, sunlit);
    CY_CHECK_GT(shadowed, 4U);
    CY_CHECK_GT(sunlit, shadowed);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the device card update shades the budgeted selection and nothing else") {
    Gpu gpu;
    if (!gpu.available()) {
        return;
    }
    RoomScene room;
    const std::vector<Surfel> surfels = gi_support::room_surfels(1.0F);
    Pair pair(gpu.fixture.device(), room.field, surfels, nullptr, gi_support::room_lights(), 4);
    constexpr u32 kBudget = 48;
    const u32 live = pair.device.page_count();
    CY_REQUIRE(live > kBudget * 4U);

    u32 frames_to_cover = 0;
    for (u64 frame = 1; frame <= 16; ++frame) {
        // What the host selection would choose from the device cache's own state, computed before
        // the update — the update must shade exactly these.
        SurfaceUpdateContext context = pair.device_context(frame, kBudget);
        cy::Array<u32> expected;
        cy::rendering::gi::SurfaceUpdateReport ignored;
        CY_REQUIRE(SurfaceCache::select(pair.device.pages(), context, false, expected, ignored)
                       .has_value());
        std::vector<SurfacePage> before(pair.device.pages().begin(), pair.device.pages().end());

        const auto report = pair.device.update(context);
        CY_CHECK_EQ(report.pages_updated, kBudget);
        CY_CHECK_EQ(pair.shading.last_submission().pages, kBudget);
        CY_REQUIRE(run_frame(gpu.fixture.device(), [&](rendering::RenderGraph& graph) {
            return pair.shading.declare(graph);
        }));
        CY_CHECK_EQ(pair.device.collect(), kBudget);

        std::vector<u8> chosen(pair.device.pages().size(), 0);
        for (const u32 handle : expected) {
            chosen[handle] = 1;
        }
        u32 shaded = 0;
        u32 untouched_moved = 0;
        for (usize handle = 0; handle < before.size(); ++handle) {
            const SurfacePage& now = pair.device.pages()[handle];
            if (now.last_update_frame == frame) {
                shaded += 1;
                CY_CHECK_EQ(chosen[handle], 1U);
            } else if (now.direct.x != before[handle].direct.x ||
                       now.accumulated.x != before[handle].accumulated.x ||
                       now.valid != before[handle].valid) {
                untouched_moved += 1;
            }
        }
        CY_CHECK_EQ(shaded, kBudget);
        CY_CHECK_EQ(untouched_moved, 0U);
        if (frames_to_cover == 0 && pair.device.diagnostics().valid_pages == live) {
            frames_to_cover = static_cast<u32>(frame);
        }
    }
    // Invalid pages outrank valid ones, so the first frames shade every page once before any page
    // twice: the whole cache is valid after exactly ceil(live / budget) frames.
    std::fprintf(stderr, "budget %u over %u pages: every page valid after %u frames\n", kBudget,
                 live, frames_to_cover);
    CY_CHECK_EQ(frames_to_cover, (live + kBudget - 1U) / kBudget);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

// --- The published picture -----------------------------------------------------------------------

namespace {

/// The cards of a cache splatted as discs through a pinhole camera, tonemapped. Not a renderer: a
/// view of what the cache holds, which is what a debug view of the surface cache is.
void splat(const SurfaceCache& cache, u32 width, u32 height, u32 x_offset, u32 stride,
           std::vector<u32>& texels, std::vector<f32>& depth) {
    const Vec3 eye{5.5F, 4.0F, 7.0F};
    const Vec3 target{0.0F, 0.6F, -0.8F};
    const Vec3 forward = normalized_or(target - eye, Vec3{0.0F, 0.0F, -1.0F});
    const Vec3 right =
        normalized_or(cross(forward, Vec3{0.0F, 1.0F, 0.0F}), Vec3{1.0F, 0.0F, 0.0F});
    const Vec3 up = cross(right, forward);
    const f32 focal = static_cast<f32>(height) * 1.1F;
    for (const SurfacePage& page : cache.pages()) {
        if (!page.live) {
            continue;
        }
        const Vec3 relative = page.position - eye;
        const f32 z = dot(relative, forward);
        if (z <= 0.1F) {
            continue;
        }
        const f32 sx = (static_cast<f32>(width) * 0.5F) + (dot(relative, right) / z * focal);
        const f32 sy = (static_cast<f32>(height) * 0.5F) - (dot(relative, up) / z * focal);
        const f32 radius = std::max(1.0F, std::sqrt(page.area) * 0.62F / z * focal);
        const Vec3 radiance = SurfaceCache::outgoing(page);
        const auto channel = [](f32 value) {
            const f32 mapped = std::pow(value / (1.0F + value), 1.0F / 2.2F);
            return static_cast<u32>(std::lround(std::clamp(mapped, 0.0F, 1.0F) * 255.0F));
        };
        const u32 colour = channel(radiance.x) | (channel(radiance.y) << 8U) |
                           (channel(radiance.z) << 16U) | 0xFF000000U;
        const i32 x0 = std::max(0, static_cast<i32>(sx - radius));
        const i32 x1 = std::min(static_cast<i32>(width) - 1, static_cast<i32>(sx + radius));
        const i32 y0 = std::max(0, static_cast<i32>(sy - radius));
        const i32 y1 = std::min(static_cast<i32>(height) - 1, static_cast<i32>(sy + radius));
        for (i32 y = y0; y <= y1; ++y) {
            for (i32 x = x0; x <= x1; ++x) {
                const f32 dx = static_cast<f32>(x) + 0.5F - sx;
                const f32 dy = static_cast<f32>(y) + 0.5F - sy;
                if ((dx * dx) + (dy * dy) > radius * radius) {
                    continue;
                }
                const usize at =
                    (static_cast<usize>(y) * stride) + x_offset + static_cast<usize>(x);
                if (z < depth[at]) {
                    depth[at] = z;
                    texels[at] = colour;
                }
            }
        }
    }
}

}  // namespace

CY_TEST_CASE("the lit surface cache cards, host and device side by side") {
    Gpu gpu;
    if (!gpu.available()) {
        return;
    }
    CourtyardScene courtyard;
    Pair pair(gpu.fixture.device(), courtyard.field, courtyard.surfels, &courtyard.shadow,
              {courtyard.sun, courtyard.lamp}, 16);
    for (u64 frame = 1; frame <= 4; ++frame) {
        CY_REQUIRE(pair.step(gpu.fixture.device(), frame, 0, true));
    }
    constexpr u32 kWidth = 480;
    constexpr u32 kHeight = 320;
    constexpr u32 kGap = 8;
    constexpr u32 kStride = (kWidth * 2U) + kGap;
    std::vector<u32> texels(static_cast<usize>(kStride) * kHeight, 0xFF201A18U);
    std::vector<f32> depth(texels.size(), 1.0e9F);
    splat(pair.host, kWidth, kHeight, 0, kStride, texels, depth);
    splat(pair.device, kWidth, kHeight, kWidth + kGap, kStride, texels, depth);

    // The two halves are the same picture: count texels that differ by more than two steps.
    u32 differing = 0;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const u32 a = texels[(static_cast<usize>(y) * kStride) + x];
            const u32 b = texels[(static_cast<usize>(y) * kStride) + kWidth + kGap + x];
            for (u32 shift = 0; shift < 24; shift += 8) {
                const i32 da = static_cast<i32>((a >> shift) & 0xFFU);
                const i32 db = static_cast<i32>((b >> shift) & 0xFFU);
                if (std::abs(da - db) > 2) {
                    differing += 1;
                    break;
                }
            }
        }
    }
    std::fprintf(stderr, "side by side: %u of %u texels differ by more than two steps\n", differing,
                 kWidth * kHeight);
    CY_CHECK_LE(differing, kWidth * kHeight / 100U);

    render_test::Image image(allocator());
    CY_REQUIRE(
        render_test::adopt(image, {texels.data(), texels.size()}, kStride, kHeight).has_value());
    const char* path = CY_TEST_BINARY_DIR "/gi-gpu-surface-cache.png";
    CY_REQUIRE(render_test::write_png(path, image).has_value());
    std::fprintf(stderr, "wrote %s (left: host surface cache, right: device)\n", path);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}
