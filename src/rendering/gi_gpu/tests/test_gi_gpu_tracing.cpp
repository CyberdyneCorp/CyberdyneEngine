// SPDX-License-Identifier: MIT
// The radiance cache and the tracing tiers on a device, against the host implementation they
// transcribe. Issue #35, stage 3.
//
// ================================================================================================
// WHAT THIS SUITE ASSERTS
// ================================================================================================
//
//   structures   the acceleration service's decisions built on the device: a build, a still frame
//                that records nothing, a moved instance that UPDATES the top level, a pose that
//                REFITS a bottom level in place, a new instance that rebuilds the top level, an
//                eviction — and after each, ray queries that agree with `AccelerationService::
//                trace` ray for ray, which is what proves the update took effect;
//   the gather   the device radiance cache against the host one, probe for probe, over a frozen
//                surface cache: every encoding, through the software tier and through the hardware
//                tier, frame after frame;
//   the query    `RadianceCache::sample` on the device against the host's, point for point;
//   the tiers    the screen tier against `ScreenTracer::trace`, the pyramid-accelerated march
//                against the unaccelerated one BIT FOR BIT with fewer reads, the software tier
//                against `SoftwareTracer`, and the escalation against `TieredTracer`, budget
//                included;
//   the fallback the hardware and software tiers on the device within `test_fallback.cpp`'s
//                tolerance; switching ray queries off mid-run moving the image no more than an
//                ordinary frame does; and a DEVICE CREATED WITHOUT RAY QUERIES taking the software
//                path with the same answer, bit for bit;
//   the cost     the gather at the GI budget's default rate and the escalation at 1080p, timed
//                with timestamps around the dispatch alone.
//
// FLOATING POINT is bounded as `render.gi_gpu` bounds it: a driver may fuse a multiply and an add,
// so the bounds are on the fraction of answers that differ beyond a small tolerance and on the
// typical difference, printed beside each.
//
// ================================================================================================
// THE MUTATIONS THIS SUITE IS PROVED AGAINST
// ================================================================================================
//
// Each applied, run on the RTX 5060, restored and md5-verified; the record with every case each one
// turns red is openspec/changes/add-gpu-gi-radiance-tracing/evidence/falsification.txt.

#include <cy/backends/rhi/device.h>
#include <cy/backends/shader/reflection.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi/bake.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/radiance_cache.h>
#include <cy/rendering/gi/surface_cache.h>
#include <cy/rendering/gi/system.h>
#include <cy/rendering/gi/tracing.h>
#include <cy/rendering/gi_gpu/gpu_radiance_cache.h>
#include <cy/rendering/gi_gpu/gpu_scene.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/raytracing/capability.h>
#include <cy/rendering/raytracing_gpu/device_structures.h>
#include <cy/test/test.h>

#include "device.h"
#include "gi_gpu_spirv.h"
#include "golden.h"
#include "support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <vector>

using namespace cy;
namespace gi = cy::rendering::gi;
namespace rt = cy::rendering::rt;
namespace rt_gpu = cy::rendering::rt_gpu;
using cy::rendering::gi_gpu::GpuDispatchTime;
using cy::rendering::gi_gpu::GpuGiScene;
using cy::rendering::gi_gpu::GpuGiSceneDescription;
using cy::rendering::gi_gpu::GpuProbeQuery;
using cy::rendering::gi_gpu::GpuRadianceCache;
using cy::rendering::gi_gpu::GpuRadianceCacheDescription;
using cy::rendering::gi_gpu::GpuTierCounters;
using cy::rendering::gi_gpu::GpuTierMode;
using cy::rendering::gi_gpu::GpuTierRay;
using cy::rendering::gi_gpu::GpuTierTracer;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

/// The tolerance `test_fallback.cpp` states for the software tier against the hardware tier: the
/// mean relative difference of the resolved indirect diffuse over its sample grid.
constexpr f32 kTierTolerance = 0.10F;

/// How much more the frame that switches the tier may move the image than an ordinary frame:
/// `test_fallback.cpp`'s `kSwitchOverControl`, the same comparison on the host.
constexpr f32 kSwitchOverControl = 1.5F;

/// A device, or a loud skip. The message is the one the suite's SKIP_REGULAR_EXPRESSION matches.
struct Gpu {
    render_test::DeviceFixture fixture;

    explicit Gpu(bool ray_tracing = true)
        : fixture("vulkan", "render.gi_gpu_tracing", true, ray_tracing) {}

    [[nodiscard]] bool available() const noexcept {
        if (fixture.is(rhi::BackendKind::Vulkan) && GpuGiScene::supported(fixture.device())) {
            return true;
        }
        std::fprintf(stderr, "no requested graphics device on this machine; ");
        fixture.report_skip();
        return false;
    }

    /// Whether the device traces rays in hardware. A case that needs it says so and returns.
    [[nodiscard]] bool ray_queries() const noexcept {
        if (rt_gpu::DeviceStructures::supported(fixture.device())) {
            return true;
        }
        std::fprintf(stderr,
                     "this device reports no ray queries (%s); the hardware-tier case is "
                     "skipped and the software cases still run\n",
                     rt::ray_tracing_refusal(fixture.device().capabilities()));
        return false;
    }

    [[nodiscard]] rhi::Device& device() const noexcept { return fixture.device(); }
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
    if (Status idle = device.wait_idle(); !idle) {
        std::fprintf(stderr, "wait_idle failed: %s\n", idle.error().message);
        (void)device.end_frame();
        return false;
    }
    if (Status ended = device.end_frame(); !ended) {
        std::fprintf(stderr, "end_frame failed: %s\n", ended.error().message);
        return false;
    }
    return true;
}

[[nodiscard]] f64 relative(Vec3 a, Vec3 b) noexcept {
    const f64 scale = std::max({static_cast<f64>(length(a)), static_cast<f64>(length(b)), 1.0e-3});
    return static_cast<f64>(length(a - b)) / scale;
}

/// How closely two sets of answers agree: the mean and worst relative difference, and how many
/// exceed a stated tolerance.
struct Agreement {
    u32 compared = 0;
    u32 beyond = 0;
    f64 mean = 0.0;
    f64 worst = 0.0;

    void add(f64 difference, f64 tolerance) noexcept {
        mean += difference;
        worst = std::max(worst, difference);
        beyond += difference > tolerance ? 1U : 0U;
        compared += 1;
    }
    void finish() noexcept { mean = compared == 0 ? 0.0 : mean / compared; }
    void print(const char* what) const {
        std::fprintf(stderr, "%s: %u compared, mean relative %.3g, worst %.3g, %u beyond\n", what,
                     compared, mean, worst, beyond);
    }
};

[[nodiscard]] GpuGiSceneDescription scene_description() noexcept {
    GpuGiSceneDescription description;
    description.max_levels = 2;
    description.max_window_bricks = 16;
    description.max_brick_slots = 16384;
    description.max_cards = 2048;
    description.max_selection = 2048;
    return description;
}

[[nodiscard]] rt::AccelerationService::ServiceConfig active_service() noexcept {
    rt::AccelerationService::ServiceConfig config;
    config.device_supports_ray_tracing = true;
    config.budget.max_builds_per_frame = 8;
    return config;
}

// --- A frozen room: the field, a host-shaded surface cache, and the service over its triangles
// ----

/// The host GI suites' room with its divider, every input the gather reads held still: the field,
/// a surface cache shaded on the host twice with direct light and nothing else, and the room's
/// triangles in an active acceleration service. What a gather comparison needs is identical
/// inputs on both sides, and a surface cache that moved under one of them would not be.
struct FrozenRoom {
    static constexpr f32 kLookupRadius = 0.6F;

    gi_support::RoomField room{Vec3{gi_support::kRoomX, gi_support::kRoomY, gi_support::kRoomZ}};
    gi_support::RoomTriangles triangles{true};
    std::vector<gi::Surfel> surfels = gi_support::room_surfels(1.0F, true);
    std::vector<gi::GiLight> lights = gi_support::room_lights();
    gi::DistanceField field;
    gi::SurfaceCache surfaces;
    gi::SoftwareTracer software;
    rt::AccelerationService service{active_service()};
    gi::HardwareTracer hardware;

    explicit FrozenRoom(const gi::ClipmapSettings& clipmaps = gi_support::room_settings().field) {
        CY_REQUIRE(field.configure(clipmaps).has_value());
        CY_REQUIRE(field.place(1, room.asset(), Mat4::identity()).has_value());
        (void)field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});
        // Off the lattice the cards stand on: see `render.gi_gpu`'s pair for the ties a half-metre
        // radius lands exactly on.
        surfaces.set_lookup_radius(kLookupRadius);
        for (const gi::Surfel& surfel : surfels) {
            CY_REQUIRE(surfaces.allocate(surfel).has_value());
        }
        software.bind(field, &surfaces);
        for (u64 frame = 1; frame <= 2; ++frame) {
            gi::SurfaceUpdateContext context;
            context.lights = {lights.data(), lights.size()};
            context.occluder = &software;
            context.frame = frame;
            (void)surfaces.update_all(context);
        }
        CY_REQUIRE(service.declare_geometry(1, rt::adapt_static_mesh(triangles.geometry(), 0))
                       .has_value());
        rt::InstanceDescriptor instance;
        instance.geometry = 1;
        instance.instance_id = 1;
        CY_REQUIRE(service.add_instance(instance).has_value());
        (void)service.update();
        hardware.bind(service, &surfaces);
    }
};

[[nodiscard]] gi::ProbeCacheSettings probe_settings(gi::ProbeEncoding encoding) noexcept {
    gi::ProbeCacheSettings settings;
    settings.levels = 2;
    settings.base_spacing_metres = 1.0F;
    settings.half_extent_probes = 4;
    settings.encoding = encoding;
    return settings;
}

/// The host radiance cache and a device-gathered one over the same frozen room.
struct ProbePair {
    FrozenRoom& room;
    gi::RadianceCache host;
    gi::RadianceCache device;
    GpuGiScene scene;
    GpuRadianceCache gpu;
    rt_gpu::DeviceStructures structures;
    bool hardware = false;

    ProbePair(rhi::Device& rhi_device, FrozenRoom& frozen, gi::ProbeEncoding encoding,
              bool with_hardware)
        : room(frozen), hardware(with_hardware) {
        CY_REQUIRE(host.configure(probe_settings(encoding)).has_value());
        CY_REQUIRE(device.configure(probe_settings(encoding)).has_value());
        gi::ProbePlacementContext placement;
        placement.field = &room.field;
        placement.lights = {room.lights.data(), room.lights.size()};
        (void)host.scroll_to(Vec3{0.0F, 0.0F, 0.0F}, placement);
        (void)device.scroll_to(Vec3{0.0F, 0.0F, 0.0F}, placement);

        CY_REQUIRE(scene.create(allocator(), rhi_device, scene_description()).has_value());
        CY_REQUIRE(
            gpu.create(allocator(), rhi_device, scene, GpuRadianceCacheDescription{}).has_value());
        gpu.bind_field(room.field);
        gpu.bind_host_surfaces(&room.surfaces);
        if (hardware) {
            CY_REQUIRE(structures.create(allocator(), rhi_device, {}).has_value());
            // Installed after the service built the room: the next update catches the mirror up.
            room.service.set_mirror(&structures);
            (void)room.service.update();
            CY_REQUIRE(run_frame(rhi_device, [&](rendering::RenderGraph& graph) {
                return structures.declare(graph);
            }));
            CY_REQUIRE(gpu.bind_hardware(&room.service, &structures).has_value());
            CY_REQUIRE_EQ(gpu.world_tier(), gi::RadianceSource::HardwareTrace);
        } else {
            CY_REQUIRE_EQ(gpu.world_tier(), gi::RadianceSource::SoftwareTrace);
        }
        device.set_gather_backend(&gpu);
    }

    ~ProbePair() { room.service.set_mirror(nullptr); }

    ProbePair(const ProbePair&) = delete;
    ProbePair& operator=(const ProbePair&) = delete;

    [[nodiscard]] gi::ProbeUpdateContext context(u64 frame) const noexcept {
        gi::ProbeUpdateContext context;
        if (hardware) {
            context.tracer = &room.hardware;
        } else {
            context.tracer = &room.software;
        }
        context.radiance = &room.surfaces;
        context.rays_per_probe = 16;
        context.max_ray_distance_metres = 24.0F;
        context.frame = frame;
        context.converged = true;
        return context;
    }

    /// One converged frame on both sides: the host gather, the device gather, its dispatch, and
    /// its results folded back.
    [[nodiscard]] bool step(rhi::Device& rhi_device, u64 frame) {
        (void)host.update(context(frame));
        const gi::ProbeUpdateReport report = device.update(context(frame));
        if (report.probes_updated == 0) {
            std::fprintf(stderr, "the device accepted no probes: %s\n", gpu.last_error().message);
            return false;
        }
        if (!run_frame(rhi_device,
                       [&](rendering::RenderGraph& graph) { return gpu.declare(graph); })) {
            return false;
        }
        const u32 collected = device.collect();
        if (collected != report.probes_updated) {
            std::fprintf(stderr, "the device gathered %u probes and %u came back\n",
                         report.probes_updated, collected);
            return false;
        }
        return true;
    }
};

/// Every live probe's payload, axis distances and error, host against device.
struct GatherAgreement {
    Agreement payload;
    f32 worst_axis = 0.0F;
    f32 worst_error = 0.0F;
    u32 validity_mismatches = 0;
};

[[nodiscard]] GatherAgreement agree(const gi::RadianceCache& host, const gi::RadianceCache& device,
                                    f64 tolerance) {
    GatherAgreement result;
    for (u32 handle = 0; handle < host.probes().size(); ++handle) {
        const gi::Probe& a = host.probe(handle);
        const gi::Probe& b = device.probe(handle);
        if (!a.live) {
            continue;
        }
        result.validity_mismatches += a.valid != b.valid ? 1U : 0U;
        const Span<const f32> pa = host.probe_payload(handle);
        const Span<const f32> pb = device.probe_payload(handle);
        f64 difference = 0.0;
        f64 magnitude_a = 0.0;
        f64 magnitude_b = 0.0;
        for (usize index = 0; index < pa.size(); ++index) {
            const f64 d = static_cast<f64>(pa[index]) - static_cast<f64>(pb[index]);
            difference += d * d;
            magnitude_a += static_cast<f64>(pa[index]) * static_cast<f64>(pa[index]);
            magnitude_b += static_cast<f64>(pb[index]) * static_cast<f64>(pb[index]);
        }
        const f64 scale = std::max({std::sqrt(magnitude_a), std::sqrt(magnitude_b), 1.0e-3});
        result.payload.add(std::sqrt(difference) / scale, tolerance);
        for (u32 axis = 0; axis < 6; ++axis) {
            result.worst_axis = std::max(result.worst_axis,
                                         std::abs(a.axis_distance[axis] - b.axis_distance[axis]));
        }
        result.worst_error = std::max(result.worst_error, std::abs(a.error - b.error));
    }
    result.payload.finish();
    return result;
}

}  // namespace

// --- The structures ------------------------------------------------------------------------------

namespace {

/// A unit box's triangles, 12 of them, for an instance that moves and a pose that refits.
struct BoxMesh {
    std::vector<Vec3> positions;
    std::vector<u32> indices;

    explicit BoxMesh(f32 half = 0.5F) {
        for (u32 corner = 0; corner < 8; ++corner) {
            positions.push_back(Vec3{(corner & 1U) != 0U ? half : -half,
                                     (corner & 2U) != 0U ? half : -half,
                                     (corner & 4U) != 0U ? half : -half});
        }
        const u32 faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
                                 {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
        for (const auto& face : faces) {
            indices.insert(indices.end(), {face[0], face[1], face[2], face[0], face[2], face[3]});
        }
    }

    [[nodiscard]] rt::TriangleGeometry geometry() const noexcept {
        rt::TriangleGeometry triangles;
        triangles.positions = {positions.data(), positions.size()};
        triangles.indices = {indices.data(), indices.size()};
        return triangles;
    }
};

/// Rays in every direction from points around the room, some of them through the box.
[[nodiscard]] std::vector<GpuTierRay> world_rays() {
    std::vector<GpuTierRay> rays;
    const Vec3 origins[] = {{0.0F, 0.0F, 2.0F},   {-2.5F, 1.0F, -2.0F}, {2.0F, -1.2F, -1.0F},
                            {3.1F, 0.4F, 3.3F},   {-0.3F, 1.6F, 0.9F},  {1.7F, -0.5F, -3.2F},
                            {-3.4F, -1.5F, 3.0F}, {0.6F, 0.1F, -0.6F}};
    constexpr u32 kPerOrigin = 128;
    for (const Vec3 origin : origins) {
        for (u32 index = 0; index < kPerOrigin; ++index) {
            const f32 y = 1.0F - (2.0F * (static_cast<f32>(index) + 0.5F) / kPerOrigin);
            const f32 radius = std::sqrt(std::max(0.0F, 1.0F - (y * y)));
            const f32 angle = 2.39996323F * static_cast<f32>(index);
            GpuTierRay ray;
            ray.origin = origin;
            ray.direction = Vec3{radius * std::cos(angle), y, radius * std::sin(angle)};
            ray.max_distance = 24.0F;
            rays.push_back(ray);
        }
    }
    return rays;
}

/// The device's hardware world tier and the host service, ray for ray: hit or miss, and distance.
struct StructureAgreement {
    u32 rays = 0;
    u32 hits = 0;
    u32 disagreements = 0;
    f32 worst_t = 0.0F;
    u32 box_hits = 0;
};

[[nodiscard]] StructureAgreement trace_structures(rhi::Device& device, GpuTierTracer& tiers,
                                                  rt::AccelerationService& service,
                                                  rt_gpu::DeviceStructures& structures,
                                                  const std::vector<GpuTierRay>& rays,
                                                  Vec3 box_centre) {
    StructureAgreement result;
    CY_REQUIRE(tiers.set_rays({rays.data(), rays.size()}).has_value());
    CY_REQUIRE(run_frame(device, [&](rendering::RenderGraph& graph) -> Status {
        if (Status built = structures.declare(graph); !built) {
            return built;
        }
        return tiers.declare(graph, GpuTierMode::World);
    }));
    std::vector<gi::RadianceSample> samples(rays.size());
    GpuTierCounters counters;
    CY_REQUIRE(tiers.read_back({samples.data(), samples.size()}, counters).has_value());
    CY_CHECK_EQ(counters.hardware_rays + counters.software_rays, 0U);  // World mode counts nothing
    for (usize index = 0; index < rays.size(); ++index) {
        rt::RayQuery query;
        query.ray.origin = rays[index].origin;
        query.ray.direction = normalized_or(rays[index].direction, Vec3{0.0F, 0.0F, -1.0F});
        query.t_max = rays[index].max_distance;
        const rt::RayHit host = service.trace(query);
        const bool device_hit = samples[index].source == gi::RadianceSource::HardwareTrace;
        result.rays += 1;
        if (host.hit != device_hit) {
            result.disagreements += 1;
            continue;
        }
        if (host.hit) {
            result.hits += 1;
            result.worst_t =
                std::max(result.worst_t, std::abs(host.t - samples[index].hit_distance));
            if (distance(host.position, box_centre) < 1.0F) {
                result.box_hits += 1;
            }
        }
    }
    std::fprintf(stderr,
                 "  structures: %u rays, %u hits (%u on the box), %u disagreements, worst |dt| "
                 "%.3g m\n",
                 result.rays, result.hits, result.box_hits, result.disagreements,
                 static_cast<double>(result.worst_t));
    return result;
}

}  // namespace

CY_TEST_CASE(
    "the device builds, refits and updates exactly what the acceleration service decided") {
    Gpu gpu;
    if (!gpu.available() || !gpu.ray_queries()) {
        return;
    }
    rhi::Device& device = gpu.device();
    FrozenRoom room;
    BoxMesh box;
    rt_gpu::DeviceStructures structures;
    CY_REQUIRE(structures.create(allocator(), device, {}).has_value());
    room.service.set_mirror(&structures);

    // A skinned box in the room: Refit maintenance, so a pose is an in-place update on the device.
    CY_REQUIRE(
        room.service.declare_geometry(2, rt::adapt_skinned_mesh(box.geometry())).has_value());
    rt::InstanceDescriptor instance;
    instance.geometry = 2;
    instance.instance_id = 2;
    Vec3 centre{0.5F, -0.4F, 1.2F};
    instance.transform = Mat4::from_translation(centre);
    const auto handle = room.service.add_instance(instance);
    CY_REQUIRE(handle.has_value());

    GpuGiScene scene;
    CY_REQUIRE(scene.create(allocator(), device, scene_description()).has_value());
    GpuRadianceCache cache;
    CY_REQUIRE(cache.create(allocator(), device, scene, GpuRadianceCacheDescription{}).has_value());
    CY_REQUIRE(
        scene.upload_cards(room.surfaces.pages(), room.surfaces.lookup_radius()).has_value());
    CY_REQUIRE(scene.upload_card_radiance(room.surfaces.pages()).has_value());
    CY_REQUIRE(cache.bind_hardware(&room.service, &structures).has_value());
    GpuTierTracer tiers(cache);
    const std::vector<GpuTierRay> rays = world_rays();

    // Frame one: the room (caught up) and the box built, and a top level over both.
    (void)room.service.update();
    StructureAgreement built =
        trace_structures(device, tiers, room.service, structures, rays, centre);
    CY_CHECK_EQ(structures.last_frame().bottom_builds, 2U);
    CY_CHECK_EQ(structures.last_frame().top_builds, 1U);
    CY_CHECK_EQ(structures.last_frame().traced_instances, 2U);
    CY_CHECK_EQ(built.disagreements, 0U);
    CY_CHECK_GT(built.box_hits, 0U);
    CY_CHECK_LT(built.worst_t, 1.0e-3F);
    CY_CHECK_EQ(cache.world_tier(), gi::RadianceSource::HardwareTrace);

    // A still frame records nothing at all.
    (void)room.service.update();
    (void)trace_structures(device, tiers, room.service, structures, rays, centre);
    CY_CHECK_EQ(structures.last_frame().bottom_builds + structures.last_frame().bottom_updates, 0U);
    CY_CHECK_EQ(structures.last_frame().top_builds + structures.last_frame().top_updates, 0U);
    CY_CHECK_EQ(structures.last_frame().top_unchanged, 1U);

    // The box moves: no bottom level is touched and the top level is UPDATED, not rebuilt — and the
    // rays now meet the box where the host meets it.
    centre = Vec3{-1.5F, 0.3F, 2.2F};
    CY_REQUIRE(room.service.set_instance_transform(handle.value(), Mat4::from_translation(centre))
                   .has_value());
    (void)room.service.update();
    const StructureAgreement moved =
        trace_structures(device, tiers, room.service, structures, rays, centre);
    CY_CHECK_EQ(structures.last_frame().bottom_builds + structures.last_frame().bottom_updates, 0U);
    CY_CHECK_EQ(structures.last_frame().top_updates, 1U);
    CY_CHECK_EQ(structures.last_frame().top_builds, 0U);
    CY_CHECK_EQ(moved.disagreements, 0U);
    CY_CHECK_GT(moved.box_hits, 0U);
    CY_CHECK_LT(moved.worst_t, 1.0e-3F);

    // A pose inside the quality floor: the service refits, and the device UPDATES the box's bottom
    // level in place and the top level over it.
    std::vector<Vec3> posed = box.positions;
    for (Vec3& position : posed) {
        position = position * 1.25F;
    }
    CY_REQUIRE(room.service.refit_geometry(2, {posed.data(), posed.size()}).has_value());
    const rt::FrameReport refit = room.service.update();
    CY_CHECK_EQ(refit.refits, 1U);
    const StructureAgreement refitted =
        trace_structures(device, tiers, room.service, structures, rays, centre);
    CY_CHECK_EQ(structures.last_frame().bottom_updates, 1U);
    CY_CHECK_EQ(structures.last_frame().bottom_builds, 0U);
    CY_CHECK_EQ(structures.last_frame().top_updates, 1U);
    CY_CHECK_EQ(refitted.disagreements, 0U);
    CY_CHECK_LT(refitted.worst_t, 1.0e-3F);

    // A second instance of the box: the instance set changed, so the top level is BUILT.
    rt::InstanceDescriptor second = instance;
    second.instance_id = 3;
    second.transform = Mat4::from_translation(Vec3{2.0F, 0.5F, -2.0F});
    const auto second_handle = room.service.add_instance(second);
    CY_REQUIRE(second_handle.has_value());
    (void)room.service.update();
    const StructureAgreement added =
        trace_structures(device, tiers, room.service, structures, rays, centre);
    CY_CHECK_EQ(structures.last_frame().top_builds, 1U);
    CY_CHECK_EQ(structures.last_frame().bottom_builds, 0U);
    CY_CHECK_EQ(structures.last_frame().traced_instances, 3U);
    CY_CHECK_EQ(added.disagreements, 0U);

    // Both boxes gone and the geometry released: the device structure is evicted with it.
    room.service.remove_instance(handle.value());
    room.service.remove_instance(second_handle.value());
    room.service.release_geometry(2);
    (void)room.service.update();
    const StructureAgreement evicted =
        trace_structures(device, tiers, room.service, structures, rays, centre);
    CY_CHECK_EQ(structures.last_frame().bottom_evictions, 1U);
    CY_CHECK_EQ(structures.last_frame().traced_instances, 1U);
    CY_CHECK_EQ(evicted.disagreements, 0U);
    CY_CHECK_EQ(evicted.box_hits, 0U);
    room.service.set_mirror(nullptr);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("a device without ray queries refuses structures and takes the software tier") {
    Gpu gpu(false);
    if (!gpu.available()) {
        return;
    }
    rhi::Device& device = gpu.device();
    // The same hardware, created without the extensions: the capability follows what was enabled.
    CY_CHECK_FALSE(device.capabilities().has(rhi::Capability::RayTracing));
    rt_gpu::DeviceStructures structures;
    const Status refused = structures.create(allocator(), device, {});
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::Unsupported);
    rhi::AccelerationStructureDescription description;
    description.kind = rhi::AccelerationStructureKind::TopLevel;
    description.max_instances = 4;
    const auto structure = device.create_acceleration_structure(description);
    CY_REQUIRE_FALSE(structure.has_value());
    CY_CHECK_EQ(structure.error().code, ErrorCode::Unsupported);

    // The radiance cache creates its software entry points only, and selects the software tier
    // whatever the service says.
    GpuGiScene scene;
    CY_REQUIRE(scene.create(allocator(), device, scene_description()).has_value());
    GpuRadianceCache cache;
    CY_REQUIRE(cache.create(allocator(), device, scene, GpuRadianceCacheDescription{}).has_value());
    CY_CHECK_FALSE(cache.hardware_supported());
    rt::AccelerationService service(rt::service_config_for(device.capabilities(), true));
    CY_CHECK_EQ(service.availability(), rt::Availability::Unsupported);
    CY_REQUIRE(cache.bind_hardware(&service, nullptr).has_value());
    CY_CHECK_EQ(cache.world_tier(), gi::RadianceSource::SoftwareTrace);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

// --- The gather and the query --------------------------------------------------------------------

namespace {

/// Payloads may differ by a driver's fused multiply-add — and, rarely, by a ray that grazes a
/// surface and is decided by one step of the march. The tolerance is per probe; the bound is on
/// how many probes exceed it and on the mean.
constexpr f64 kPayloadTolerance = 1.0e-3;
constexpr f64 kPayloadMean = 1.0e-3;
constexpr f64 kPayloadBeyondFraction = 0.02;
/// The visibility term: how far each probe sees along each axis, in metres.
constexpr f32 kAxisTolerance = 1.0e-2F;
/// A probe's error, which is already a relative change.
constexpr f32 kErrorTolerance = 1.0e-3F;

void check_gather(const char* what, const GatherAgreement& agreement) {
    agreement.payload.print(what);
    std::fprintf(stderr,
                 "    worst axis distance %.3g m, worst error %.3g, %u validity mismatches\n",
                 static_cast<double>(agreement.worst_axis),
                 static_cast<double>(agreement.worst_error), agreement.validity_mismatches);
    CY_CHECK_GT(agreement.payload.compared, 50U);
    CY_CHECK_EQ(agreement.validity_mismatches, 0U);
    CY_CHECK_LT(agreement.payload.mean, kPayloadMean);
    CY_CHECK_LE(static_cast<f64>(agreement.payload.beyond),
                static_cast<f64>(agreement.payload.compared) * kPayloadBeyondFraction);
    CY_CHECK_LT(agreement.worst_axis, kAxisTolerance);
    // The error the scheduler ranks on is the change from the PREVIOUS payload, so it is also the
    // check that the device's previous payload was the host's.
    CY_CHECK_LT(agreement.worst_error, kErrorTolerance);
}

}  // namespace

CY_TEST_CASE("the device gather matches the host radiance cache, every encoding, software tier") {
    Gpu gpu;
    if (!gpu.available()) {
        return;
    }
    FrozenRoom room;
    for (const gi::ProbeEncoding encoding :
         {gi::ProbeEncoding::SphericalHarmonicsL1, gi::ProbeEncoding::Octahedral,
          gi::ProbeEncoding::SphericalGaussian}) {
        ProbePair pair(gpu.device(), room, encoding, false);
        // Six frames: the sequence rotates every frame, so each frame traces different rays, and
        // the error the scheduler ranks on is compared after it has a previous frame to differ
        // from.
        for (u64 frame = 1; frame <= 6; ++frame) {
            CY_REQUIRE(pair.step(gpu.device(), frame));
            CY_CHECK_EQ(pair.gpu.last_submission().tier, gi::RadianceSource::SoftwareTrace);
        }
        char label[96];
        std::snprintf(label, sizeof(label), "gather, software tier, %s",
                      gi::probe_encoding_name(encoding));
        check_gather(label, agree(pair.host, pair.device, kPayloadTolerance));
    }
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the device gather matches the host radiance cache through the hardware tier") {
    Gpu gpu;
    if (!gpu.available() || !gpu.ray_queries()) {
        return;
    }
    FrozenRoom room;
    ProbePair pair(gpu.device(), room, gi::ProbeEncoding::SphericalHarmonicsL1, true);
    const u64 rays_before =
        room.service.diagnostics().rays[static_cast<u32>(rt::Consumer::GlobalIllumination)];
    for (u64 frame = 1; frame <= 6; ++frame) {
        CY_REQUIRE(pair.step(gpu.device(), frame));
        CY_CHECK_EQ(pair.gpu.last_submission().tier, gi::RadianceSource::HardwareTrace);
    }
    // The host side really traced the hardware tier: without this, both could be the software one.
    CY_CHECK_GT(room.service.diagnostics().rays[static_cast<u32>(rt::Consumer::GlobalIllumination)],
                rays_before);
    check_gather("gather, hardware tier, SphericalHarmonicsL1",
                 agree(pair.host, pair.device, kPayloadTolerance));
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the device query answers as RadianceCache::sample does") {
    Gpu gpu;
    if (!gpu.available()) {
        return;
    }
    FrozenRoom room;
    ProbePair pair(gpu.device(), room, gi::ProbeEncoding::SphericalHarmonicsL1, false);
    for (u64 frame = 1; frame <= 3; ++frame) {
        CY_REQUIRE(pair.step(gpu.device(), frame));
    }
    // Every point a resolve might ask about: the floor, the ceiling, both faces of the divider and
    // the four walls, on a grid that crosses probe cells, the doorway and the divider itself.
    std::vector<GpuProbeQuery> queries;
    for (i32 ix = -7; ix <= 7; ++ix) {
        for (i32 iz = -7; iz <= 7; ++iz) {
            const f32 x = static_cast<f32>(ix) * 0.53F;
            const f32 z = static_cast<f32>(iz) * 0.53F;
            queries.push_back({Vec3{x, -gi_support::kRoomY + 0.1F, z}, Vec3{0.0F, 1.0F, 0.0F}});
            queries.push_back({Vec3{x, gi_support::kRoomY - 0.1F, z}, Vec3{0.0F, -1.0F, 0.0F}});
            queries.push_back({Vec3{x, 0.3F, 0.05F}, Vec3{0.0F, 0.0F, 1.0F}});
            queries.push_back({Vec3{x, 0.3F, -0.05F}, Vec3{0.0F, 0.0F, -1.0F}});
        }
    }
    const u64 frame = 9;
    CY_REQUIRE(pair.gpu.upload(pair.device).has_value());
    CY_REQUIRE(pair.gpu.set_queries({queries.data(), queries.size()}).has_value());
    CY_REQUIRE(run_frame(gpu.device(), [&](rendering::RenderGraph& graph) {
        return pair.gpu.declare_sample(graph, frame);
    }));
    std::vector<gi::RadianceSample> device(queries.size());
    CY_REQUIRE(pair.gpu.read_back_samples({device.data(), device.size()}).has_value());

    // The device query against the host query over the SAME probes (the device-gathered cache), so
    // what is compared is the query alone.
    Agreement radiance;
    u32 confidence_mismatches = 0;
    u32 answered = 0;
    for (usize index = 0; index < queries.size(); ++index) {
        const gi::RadianceSample host =
            pair.device.sample(queries[index].position, queries[index].normal, frame);
        radiance.add(relative(host.radiance, device[index].radiance), 1.0e-4);
        confidence_mismatches +=
            std::abs(host.confidence - device[index].confidence) > 1.0e-5F ? 1U : 0U;
        answered += host.confidence > 0.0F ? 1U : 0U;
    }
    radiance.finish();
    radiance.print("query");
    std::fprintf(stderr, "    %u of %zu points answered, %u confidence mismatches\n", answered,
                 queries.size(), confidence_mismatches);
    CY_CHECK_GT(answered, static_cast<u32>(queries.size() / 2));
    CY_CHECK_EQ(confidence_mismatches, 0U);
    CY_CHECK_LT(radiance.mean, 1.0e-5);
    CY_CHECK_LT(radiance.worst, 1.0e-3);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("a probe the host gathers again reaches the device at the next upload") {
    // REGRESSION, issue #35 stage 4. A gather is not a change the host makes to a probe, so it
    // moves no revision — and a mirror that keyed its uploads on revision alone uploaded a
    // host-gathered probe once and served that first payload to every device query after it.
    // The per-pixel resolve queries a cache the host gathers, and read probes frames stale.
    Gpu gpu;
    if (!gpu.available()) {
        return;
    }
    FrozenRoom room;
    ProbePair pair(gpu.device(), room, gi::ProbeEncoding::SphericalHarmonicsL1, false);
    // The HOST cache, gathered on the host: the device only mirrors it.
    for (u64 frame = 1; frame <= 2; ++frame) {
        (void)pair.host.update(pair.context(frame));
    }
    std::vector<GpuProbeQuery> queries;
    for (i32 ix = -6; ix <= 6; ++ix) {
        for (i32 iz = -6; iz <= 6; ++iz) {
            queries.push_back({Vec3{static_cast<f32>(ix) * 0.61F, -gi_support::kRoomY + 0.1F,
                                    static_cast<f32>(iz) * 0.61F},
                               Vec3{0.0F, 1.0F, 0.0F}});
        }
    }
    CY_REQUIRE(pair.gpu.set_queries({queries.data(), queries.size()}).has_value());
    std::vector<gi::RadianceSample> device(queries.size());
    const auto query = [&](u64 frame) {
        CY_REQUIRE(pair.gpu.upload(pair.host).has_value());
        CY_REQUIRE(run_frame(gpu.device(), [&](rendering::RenderGraph& graph) {
            return pair.gpu.declare_sample(graph, frame);
        }));
        CY_REQUIRE(pair.gpu.read_back_samples({device.data(), device.size()}).has_value());
        Agreement radiance;
        for (usize index = 0; index < queries.size(); ++index) {
            const gi::RadianceSample host =
                pair.host.sample(queries[index].position, queries[index].normal, frame);
            radiance.add(relative(host.radiance, device[index].radiance), 1.0e-4);
        }
        radiance.finish();
        return radiance;
    };
    const Agreement first = query(3);
    first.print("first upload");
    // Gathered again on the host — the rays rotate, so every payload moves — and uploaded again.
    for (u64 frame = 3; frame <= 5; ++frame) {
        (void)pair.host.update(pair.context(frame));
    }
    const Agreement again = query(6);
    again.print("after three more host gathers");
    CY_CHECK_LT(first.mean, 1.0e-5);
    CY_CHECK_LT(again.mean, 1.0e-5);
    CY_CHECK_LT(again.worst, 1.0e-3);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("a reconfigured radiance cache is uploaded afresh, whatever its probes' revisions") {
    // REGRESSION. `RadianceCache::configure` starts the probe slots over, so the probe created in
    // slot 0 after it carries the same revision as the probe slot 0 held before — and a mirror that
    // keyed its uploads on revision alone kept the old probe's payload, in the old encoding's
    // stride. The cache's `generation()` moves at every configure and the mirror starts over when
    // it does.
    Gpu gpu;
    if (!gpu.available()) {
        return;
    }
    FrozenRoom room;
    ProbePair pair(gpu.device(), room, gi::ProbeEncoding::SphericalHarmonicsL1, false);
    for (u64 frame = 1; frame <= 2; ++frame) {
        CY_REQUIRE(pair.step(gpu.device(), frame));
    }
    // The same placement in another encoding: every probe comes back in its old slot.
    gi::ProbePlacementContext placement;
    placement.field = &room.field;
    placement.lights = {room.lights.data(), room.lights.size()};
    for (gi::RadianceCache* cache : {&pair.host, &pair.device}) {
        CY_REQUIRE(cache->configure(probe_settings(gi::ProbeEncoding::Octahedral)).has_value());
        (void)cache->scroll_to(Vec3{0.0F, 0.0F, 0.0F}, placement);
    }
    // ONE frame: the first gather after the reconfigure is the one whose previous payload — and so
    // whose error — is the stale probe's when the mirror kept it; a second frame overwrites it.
    CY_REQUIRE(pair.step(gpu.device(), 3));
    check_gather("gather after a reconfigure, Octahedral",
                 agree(pair.host, pair.device, kPayloadTolerance));
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

// --- The fallback, through the whole illumination system -----------------------------------------

namespace {

/// `test_fallback.cpp`'s closed room with no divider, in an `IlluminationSystem` whose radiance
/// cache is gathered on the device. `ray_tracing` asks for the hardware tier; whether the device
/// can give it is the device's answer (`rt::service_config_for`), exactly as a renderer would ask.
struct DeviceRoom {
    gi_support::RoomField field{Vec3{gi_support::kRoomX, gi_support::kRoomY, gi_support::kRoomZ}};
    gi_support::RoomTriangles triangles{false};
    std::vector<gi::Surfel> surfels = gi_support::room_surfels(1.0F, false);
    std::vector<gi::GiLight> lights = gi_support::room_lights();
    gi::IlluminationSystem system;
    GpuGiScene scene;
    GpuRadianceCache radiance;
    rt_gpu::DeviceStructures structures;
    rhi::Device* device = nullptr;
    bool on_device = true;

    /// `device` null is the host-gathered control: the same room with no backend installed.
    DeviceRoom(rhi::Device* rhi_device, bool ray_tracing) : device(rhi_device) {
        gi::IlluminationSettings settings = gi_support::room_settings();
        if (device != nullptr) {
            settings.ray_tracing = rt::service_config_for(device->capabilities(), ray_tracing);
        } else {
            settings.ray_tracing.device_supports_ray_tracing = ray_tracing;
        }
        settings.ray_tracing.budget.max_builds_per_frame = 8;
        CY_REQUIRE(system.configure(settings).has_value());
        CY_REQUIRE(system.field().place(1, field.asset(), Mat4::identity()).has_value());
        const Aabb bounds = Aabb::from_center_extents(
            Vec3{0.0F, 0.0F, 0.0F},
            Vec3{gi_support::kRoomX + 1.0F, gi_support::kRoomY + 1.0F, gi_support::kRoomZ + 1.0F});
        CY_REQUIRE(
            system.scene().ingest_cell(1, bounds, {surfels.data(), surfels.size()}, 0).has_value());
        CY_REQUIRE(system.surfaces().allocate_from(system.scene(), bounds).has_value());
        system.surfaces().set_lookup_radius(1.2F);
        CY_REQUIRE(system.acceleration()
                       .declare_geometry(1, rt::adapt_static_mesh(triangles.geometry(), 0))
                       .has_value());
        rt::InstanceDescriptor instance;
        instance.geometry = 1;
        instance.instance_id = 1;
        CY_REQUIRE(system.acceleration().add_instance(instance).has_value());
        on_device = device != nullptr;
        if (!on_device) {
            return;
        }
        CY_REQUIRE(scene.create(allocator(), *device, scene_description()).has_value());
        CY_REQUIRE(radiance.create(allocator(), *device, scene, GpuRadianceCacheDescription{})
                       .has_value());
        radiance.bind_field(system.field());
        radiance.bind_host_surfaces(&system.surfaces());
        if (ray_tracing && rt_gpu::DeviceStructures::supported(*device)) {
            CY_REQUIRE(structures.create(allocator(), *device, {}).has_value());
            system.acceleration().set_mirror(&structures);
        }
        CY_REQUIRE(
            radiance
                .bind_hardware(&system.acceleration(), structures.created() ? &structures : nullptr)
                .has_value());
        system.set_probe_gather(&radiance);
    }

    ~DeviceRoom() { system.acceleration().set_mirror(nullptr); }
    DeviceRoom(const DeviceRoom&) = delete;
    DeviceRoom& operator=(const DeviceRoom&) = delete;

    [[nodiscard]] gi::FrameContext context(u64 frame, bool converged) const noexcept {
        gi::FrameContext ctx;
        ctx.lights = {lights.data(), lights.size()};
        ctx.frame = frame;
        ctx.measured_gi_ms = 1.0F;
        ctx.converged_mode = converged;
        return ctx;
    }

    /// One frame of the system, and on the device the frame that runs what it submitted.
    [[nodiscard]] bool frame(u64 index, bool converged = true) {
        (void)system.update(context(index, converged));
        if (!on_device) {
            return true;
        }
        const bool gather = radiance.last_submission().probes != 0;
        if (!run_frame(*device, [&](rendering::RenderGraph& graph) -> Status {
                if (structures.created()) {
                    if (Status built = structures.declare(graph); !built) {
                        return built;
                    }
                }
                return gather ? radiance.declare(graph) : graph.status();
            })) {
            return false;
        }
        (void)system.radiance().collect();
        return true;
    }

    [[nodiscard]] bool run(u64 first, u64 frames) {
        for (u64 index = first; index < first + frames; ++index) {
            if (!frame(index)) {
                return false;
            }
        }
        return true;
    }
};

/// `test_fallback.cpp`'s eighty points: a grid over the floor and the ceiling and up two walls.
[[nodiscard]] std::vector<std::pair<Vec3, Vec3>> fallback_points() {
    std::vector<std::pair<Vec3, Vec3>> points;
    const auto span = [](u32 index) { return -2.5F + (static_cast<f32>(index) * 1.25F); };
    for (u32 ix = 0; ix < 5; ++ix) {
        for (u32 iz = 0; iz < 5; ++iz) {
            points.emplace_back(Vec3{span(ix), -gi_support::kRoomY + 0.1F, span(iz)},
                                Vec3{0.0F, 1.0F, 0.0F});
            points.emplace_back(Vec3{span(ix), gi_support::kRoomY - 0.1F, span(iz)},
                                Vec3{0.0F, -1.0F, 0.0F});
        }
    }
    for (u32 iy = 0; iy < 3; ++iy) {
        const f32 y = -1.0F + static_cast<f32>(iy);
        for (u32 iz = 0; iz < 5; ++iz) {
            points.emplace_back(Vec3{-gi_support::kRoomX + 0.1F, y, span(iz)},
                                Vec3{1.0F, 0.0F, 0.0F});
            points.emplace_back(Vec3{gi_support::kRoomX - 0.1F, y, span(iz)},
                                Vec3{-1.0F, 0.0F, 0.0F});
        }
    }
    return points;
}

[[nodiscard]] std::vector<Vec3> resolve_all(const gi::IlluminationSystem& system,
                                            const std::vector<std::pair<Vec3, Vec3>>& points) {
    std::vector<Vec3> values;
    values.reserve(points.size());
    const gi::SurfaceProperties surface;
    for (const auto& point : points) {
        values.push_back(system.indirect_diffuse(point.first, point.second, surface).radiance);
    }
    return values;
}

[[nodiscard]] gi::ReferenceComparison compare(const std::vector<Vec3>& measured,
                                              const std::vector<Vec3>& reference) {
    return gi::compare_against_reference({measured.data(), measured.size()},
                                         {reference.data(), reference.size()});
}

constexpr u64 kConvergedFrames = 12;

}  // namespace

CY_TEST_CASE("the hardware and software tiers on the device agree within the fallback tolerance") {
    Gpu gpu;
    if (!gpu.available() || !gpu.ray_queries()) {
        return;
    }
    DeviceRoom hardware(&gpu.device(), true);
    DeviceRoom software(&gpu.device(), false);
    DeviceRoom host(nullptr, false);
    CY_REQUIRE(hardware.run(0, kConvergedFrames));
    CY_REQUIRE(software.run(0, kConvergedFrames));
    CY_REQUIRE(host.run(0, kConvergedFrames));
    // The tiers each side really traced, said by the device rather than assumed.
    CY_CHECK_EQ(hardware.radiance.last_submission().tier, gi::RadianceSource::HardwareTrace);
    CY_CHECK_EQ(software.radiance.last_submission().tier, gi::RadianceSource::SoftwareTrace);
    CY_CHECK_GT(hardware.structures.totals().top_builds, 0U);

    const auto points = fallback_points();
    const std::vector<Vec3> on_hardware = resolve_all(hardware.system, points);
    const std::vector<Vec3> on_software = resolve_all(software.system, points);
    const std::vector<Vec3> on_host = resolve_all(host.system, points);
    const gi::ReferenceComparison tiers = compare(on_software, on_hardware);
    const gi::ReferenceComparison oracle = compare(on_software, on_host);
    std::fprintf(stderr,
                 "\n  device fallback: software vs hardware tier over %u points\n"
                 "    mean relative difference   %.4f   (tolerance %.2f, test_fallback.cpp's)\n"
                 "    max  absolute difference   %.5f\n"
                 "  device software tier vs the host-gathered room\n"
                 "    mean relative difference   %.3g\n\n",
                 tiers.samples, static_cast<double>(tiers.relative_error),
                 static_cast<double>(kTierTolerance), static_cast<double>(tiers.max_absolute_error),
                 static_cast<double>(oracle.relative_error));
    CY_CHECK_GT(tiers.mean_reference_magnitude, 0.0F);
    CY_CHECK_LT(tiers.relative_error, kTierTolerance);
    // "Probe irradiance matches the CPU radiance cache", resolved the way a frame resolves it.
    CY_CHECK_LT(oracle.relative_error, 1.0e-3F);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("switching ray queries off moves the image no more than an ordinary frame does") {
    Gpu gpu;
    if (!gpu.available() || !gpu.ray_queries()) {
        return;
    }
    DeviceRoom control(&gpu.device(), true);
    DeviceRoom switched(&gpu.device(), true);
    CY_REQUIRE(control.run(0, kConvergedFrames));
    CY_REQUIRE(switched.run(0, kConvergedFrames));
    const auto points = fallback_points();
    const std::vector<Vec3> before = resolve_all(switched.system, points);
    const std::vector<Vec3> control_before = resolve_all(control.system, points);
    // One device, one shader, the same inputs: the two rooms are the same room.
    for (usize index = 0; index < before.size(); ++index) {
        CY_CHECK_EQ(before[index].x, control_before[index].x);
    }

    // An ordinary next frame, and a next frame with ray queries switched off by the profile.
    CY_REQUIRE(control.frame(kConvergedFrames));
    switched.system.acceleration().set_enabled_by_profile(false);
    CY_CHECK_EQ(switched.radiance.world_tier(), gi::RadianceSource::SoftwareTrace);
    CY_REQUIRE(switched.frame(kConvergedFrames));
    CY_CHECK_EQ(switched.radiance.last_submission().tier, gi::RadianceSource::SoftwareTrace);
    CY_CHECK_EQ(control.radiance.last_submission().tier, gi::RadianceSource::HardwareTrace);
    // The structures are kept: a profile change is not a residency event.
    CY_CHECK_EQ(switched.structures.last_frame().bottom_evictions, 0U);

    // THREE NUMBERS, BECAUSE A HYBRID IMAGE MOVES EVERY FRAME. The probe gather rotates its ray
    // set each frame, so an ordinary frame moves the image by itself; the switching frame moves it
    // by that AND by the switch. What the switch alone did is the difference between the two rooms
    // at the same frame — one switched, one not — and that is held to a frame of motion. The raw
    // step is held to `test_fallback.cpp`'s bound for the same comparison on the host.
    const std::vector<Vec3> control_after = resolve_all(control.system, points);
    const std::vector<Vec3> after = resolve_all(switched.system, points);
    const gi::ReferenceComparison ordinary = compare(control_after, control_before);
    const gi::ReferenceComparison step = compare(after, before);
    const gi::ReferenceComparison switch_alone = compare(after, control_after);
    std::fprintf(stderr,
                 "  device profile switch: hardware -> software, one frame later\n"
                 "    an ordinary frame moves         %.4f\n"
                 "    the switch alone moves it       %.4f   (at most the ordinary frame)\n"
                 "    the switching frame moves       %.4f   (at most %.1fx the ordinary one)\n\n",
                 static_cast<double>(ordinary.relative_error),
                 static_cast<double>(switch_alone.relative_error),
                 static_cast<double>(step.relative_error), static_cast<double>(kSwitchOverControl));
    CY_CHECK_GT(ordinary.relative_error, 0.0F);
    CY_CHECK_LE(switch_alone.relative_error, ordinary.relative_error);
    CY_CHECK_LT(step.relative_error, ordinary.relative_error * kSwitchOverControl);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("a device without ray queries and a profile that disabled them draw the same room") {
    // ONE DEVICE AT A TIME. The backend loads its entry points through volk, which holds ONE
    // device's table process-wide, so the device without ray queries is created, drawn with and
    // destroyed before the one with them exists.
    const auto points = fallback_points();
    std::vector<Vec3> absent_values;
    {
        Gpu without(false);
        if (!without.available()) {
            return;
        }
        CY_REQUIRE_FALSE(without.device().capabilities().has(rhi::Capability::RayTracing));
        DeviceRoom absent(&without.device(), true);
        CY_CHECK_EQ(absent.system.acceleration().availability(), rt::Availability::Unsupported);
        CY_CHECK_FALSE(absent.structures.created());
        CY_REQUIRE(absent.run(0, kConvergedFrames));
        CY_CHECK_EQ(absent.radiance.last_submission().tier, gi::RadianceSource::SoftwareTrace);
        absent_values = resolve_all(absent.system, points);
        CY_CHECK_EQ(without.fixture.validation_errors(), 0U);
    }
    Gpu with;
    if (!with.available()) {
        return;
    }
    DeviceRoom disabled(&with.device(), true);
    disabled.system.acceleration().set_enabled_by_profile(false);
    CY_REQUIRE(disabled.run(0, kConvergedFrames));
    CY_CHECK_EQ(disabled.radiance.last_submission().tier, gi::RadianceSource::SoftwareTrace);
    const std::vector<Vec3> disabled_values = resolve_all(disabled.system, points);
    CY_REQUIRE_EQ(absent_values.size(), disabled_values.size());
    // Bit for bit: the same shader over the same data on the same silicon, whichever cause.
    u32 different = 0;
    for (usize index = 0; index < absent_values.size(); ++index) {
        const Vec3 a = absent_values[index];
        const Vec3 b = disabled_values[index];
        different += (a.x != b.x || a.y != b.y || a.z != b.z) ? 1U : 0U;
    }
    std::fprintf(stderr, "  no ray queries vs disabled by profile: %u of %zu points differ\n",
                 different, absent_values.size());
    CY_CHECK_EQ(different, 0U);
    CY_CHECK_EQ(with.fixture.validation_errors(), 0U);
}

// --- The tiers -----------------------------------------------------------------------------------

namespace {

/// A view and the buffers a renderer would hand the screen tier: linear view depth and last frame's
/// colour, made by tracing a primary ray per pixel through `service` and reading `surfaces` at the
/// hit — a frame the scene could have drawn. `pitch` tilts the camera down from looking along -z.
struct ScreenFixture {
    u32 width = 0;
    u32 height = 0;
    Vec3 camera{0.3F, 0.2F, 3.6F};
    std::vector<f32> depth;
    std::vector<Vec3> normal;
    std::vector<Vec3> colour;
    std::vector<Vec3> position;
    Mat4 world_to_view = Mat4::identity();
    Mat4 world_to_clip = Mat4::identity();

    ScreenFixture(FrozenRoom& room, u32 w, u32 h)
        : ScreenFixture(room.service, &room.surfaces, w, h, Vec3{0.3F, 0.2F, 3.6F}, 0.0F) {}

    ScreenFixture(rt::AccelerationService& service, const gi::SurfaceCache* surfaces, u32 w, u32 h,
                  Vec3 eye, f32 pitch)
        : width(w), height(h), camera(eye) {
        constexpr f32 kFov = 1.2217305F;  // 70 degrees
        const f32 aspect = static_cast<f32>(w) / static_cast<f32>(h);
        const f32 c = std::cos(pitch);
        const f32 s = std::sin(pitch);
        // World to view: the camera's translation undone, then its pitch; view to world for the
        // primary rays is the inverse rotation.
        Mat4 rotation = Mat4::identity();
        rotation.columns[1] = Vec4{0.0F, c, s, 0.0F};
        rotation.columns[2] = Vec4{0.0F, -s, c, 0.0F};
        world_to_view = rotation * Mat4::from_translation(-camera);
        world_to_clip = perspective_reversed_z(kFov, aspect, 0.05F, 200.0F) * world_to_view;
        const f32 tangent = std::tan(kFov * 0.5F);
        const usize pixels = static_cast<usize>(w) * h;
        depth.assign(pixels, 0.0F);
        normal.assign(pixels, Vec3{0.0F, 0.0F, 1.0F});
        colour.assign(pixels, Vec3{0.0F, 0.0F, 0.0F});
        position.assign(pixels, camera);
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const f32 ndc_x =
                    (((static_cast<f32>(x) + 0.5F) / static_cast<f32>(w)) * 2.0F) - 1.0F;
                const f32 ndc_y =
                    1.0F - (((static_cast<f32>(y) + 0.5F) / static_cast<f32>(h)) * 2.0F);
                const Vec3 view{ndc_x * tangent * aspect, ndc_y * tangent, -1.0F};
                const Vec3 world{view.x, (view.y * c) - (view.z * s), (view.y * s) + (view.z * c)};
                rt::RayQuery query;
                query.ray.origin = camera;
                query.ray.direction = normalized_or(world, Vec3{0.0F, 0.0F, -1.0F});
                query.t_max = 200.0F;
                query.consumer = rt::Consumer::Gameplay;
                const rt::RayHit hit = service.trace(query);
                const usize at = (static_cast<usize>(y) * w) + x;
                if (!hit.hit) {
                    continue;  // sky: depth zero
                }
                depth[at] = hit.t / length(view);
                normal[at] = hit.normal;
                position[at] = hit.position;
                Vec3 radiance{0.5F, 0.5F, 0.5F};
                u32 age = 0;
                if (surfaces != nullptr) {
                    (void)surfaces->radiance_at(hit.position, hit.normal, radiance, age);
                }
                colour[at] = radiance;
            }
        }
    }

    /// The same frame at `factor` times the resolution, each pixel repeated: a 1080p depth and
    /// colour buffer for a cost measurement, from a quarter of the processor's primary rays — the
    /// case's own CPU budget is what tracing every 1080p pixel on the host would spend.
    [[nodiscard]] ScreenFixture upscaled(u32 factor) const {
        ScreenFixture out = *this;
        out.width = width * factor;
        out.height = height * factor;
        const usize pixels = static_cast<usize>(out.width) * out.height;
        out.depth.assign(pixels, 0.0F);
        out.normal.assign(pixels, Vec3{0.0F, 0.0F, 1.0F});
        out.colour.assign(pixels, Vec3{0.0F, 0.0F, 0.0F});
        out.position.assign(pixels, camera);
        for (u32 y = 0; y < out.height; ++y) {
            for (u32 x = 0; x < out.width; ++x) {
                const usize from = (static_cast<usize>(y / factor) * width) + (x / factor);
                const usize to = (static_cast<usize>(y) * out.width) + x;
                out.depth[to] = depth[from];
                out.normal[to] = normal[from];
                out.colour[to] = colour[from];
                out.position[to] = position[from];
            }
        }
        return out;
    }

    [[nodiscard]] gi::ScreenView view() const noexcept {
        gi::ScreenView screen;
        screen.width = width;
        screen.height = height;
        screen.depth = {depth.data(), depth.size()};
        screen.normal = {normal.data(), normal.size()};
        screen.colour = {colour.data(), colour.size()};
        screen.world_to_view = world_to_view;
        screen.world_to_clip = world_to_clip;
        screen.camera_position = camera;
        screen.thickness_metres = 0.3F;
        screen.max_steps = 48;
        return screen;
    }

    /// A gather ray from every `stride`-th visible pixel: off the surface, into its hemisphere.
    [[nodiscard]] std::vector<GpuTierRay> rays(u32 stride, f32 reach) const {
        std::vector<GpuTierRay> out;
        for (u32 y = 0; y < height; y += stride) {
            for (u32 x = 0; x < width; x += stride) {
                const usize at = (static_cast<usize>(y) * width) + x;
                if (depth[at] <= 0.0F) {
                    continue;
                }
                GpuTierRay ray;
                ray.origin = position[at] + (normal[at] * 0.05F);
                ray.direction =
                    gi::hemisphere_direction(normal[at], ((x / stride) + (y / stride)) % 8U, 8);
                ray.max_distance = reach;
                out.push_back(ray);
            }
        }
        return out;
    }
};

/// The device's tier answers against the host's, ray for ray.
struct TierAgreement {
    u32 rays = 0;
    u32 disagreements = 0;
    f32 worst_confidence = 0.0F;
    f64 worst_radiance = 0.0;
};

[[nodiscard]] bool same_answer(const gi::RadianceSample& host, const gi::RadianceSample& device,
                               TierAgreement& agreement) {
    const f32 confidence = std::abs(host.confidence - device.confidence);
    const f64 radiance = relative(host.radiance, device.radiance);
    const bool hit_agrees = (host.hit_distance < 0.0F) == (device.hit_distance < 0.0F) &&
                            std::abs(host.hit_distance - device.hit_distance) < 1.0e-3F;
    const bool same =
        host.source == device.source && hit_agrees && confidence < 1.0e-3F && radiance < 1.0e-3;
    agreement.rays += 1;
    if (!same) {
        agreement.disagreements += 1;
        return false;
    }
    agreement.worst_confidence = std::max(agreement.worst_confidence, confidence);
    agreement.worst_radiance = std::max(agreement.worst_radiance, radiance);
    return true;
}

void print(const char* what, const TierAgreement& agreement) {
    std::fprintf(stderr,
                 "%s: %u rays, %u disagreements, worst confidence %.3g, worst radiance %.3g\n",
                 what, agreement.rays, agreement.disagreements,
                 static_cast<double>(agreement.worst_confidence), agreement.worst_radiance);
}

/// The scene a tier batch reads, uploaded: the field, the cards and their radiance.
struct TierRig {
    GpuGiScene scene;
    GpuRadianceCache cache;
    rt_gpu::DeviceStructures structures;

    TierRig(rhi::Device& device, FrozenRoom& room, bool hardware,
            const GpuRadianceCacheDescription& description = {}) {
        CY_REQUIRE(scene.create(allocator(), device, scene_description()).has_value());
        CY_REQUIRE(cache.create(allocator(), device, scene, description).has_value());
        CY_REQUIRE(scene.upload_field(room.field).has_value());
        CY_REQUIRE(
            scene.upload_cards(room.surfaces.pages(), room.surfaces.lookup_radius()).has_value());
        CY_REQUIRE(scene.upload_card_radiance(room.surfaces.pages()).has_value());
        if (hardware) {
            CY_REQUIRE(structures.create(allocator(), device, {}).has_value());
            room.service.set_mirror(&structures);
            (void)room.service.update();
            CY_REQUIRE(run_frame(
                device, [&](rendering::RenderGraph& graph) { return structures.declare(graph); }));
            CY_REQUIRE(cache.bind_hardware(&room.service, &structures).has_value());
        }
    }
};

[[nodiscard]] bool run_tiers(rhi::Device& device, GpuTierTracer& tiers,
                             const std::vector<GpuTierRay>& rays, GpuTierMode mode,
                             std::vector<gi::RadianceSample>& out, GpuTierCounters& counters) {
    if (!tiers.set_rays({rays.data(), rays.size()}).has_value()) {
        return false;
    }
    if (!run_frame(device,
                   [&](rendering::RenderGraph& graph) { return tiers.declare(graph, mode); })) {
        return false;
    }
    out.assign(rays.size(), gi::RadianceSample{});
    return tiers.read_back({out.data(), out.size()}, counters).has_value();
}

/// The surface cache's clock in the frozen room: the frame of its last update.
constexpr u64 kCacheFrame = 2;

}  // namespace

CY_TEST_CASE("the screen tier answers as ScreenTracer does, and the pyramid changes no answer") {
    Gpu gpu;
    if (!gpu.available()) {
        return;
    }
    FrozenRoom room;
    const ScreenFixture screen(room, 256, 144);
    const gi::ScreenView view = screen.view();
    TierRig rig(gpu.device(), room, false);
    GpuTierTracer tiers(rig.cache);
    CY_REQUIRE(tiers.set_view(&view).has_value());
    const std::vector<GpuTierRay> rays = screen.rays(2, 6.0F);

    std::vector<gi::RadianceSample> plain;
    std::vector<gi::RadianceSample> accelerated;
    GpuTierCounters plain_counters;
    GpuTierCounters accelerated_counters;
    CY_REQUIRE(run_tiers(gpu.device(), tiers, rays, GpuTierMode::Screen, plain, plain_counters));
    CY_REQUIRE(run_tiers(gpu.device(), tiers, rays, GpuTierMode::ScreenAccelerated, accelerated,
                         accelerated_counters));

    TierAgreement agreement;
    u32 hits = 0;
    u32 exits = 0;
    for (usize index = 0; index < rays.size(); ++index) {
        const gi::RadianceSample host = gi::ScreenTracer::trace(
            view, rays[index].origin, rays[index].direction, rays[index].max_distance);
        (void)same_answer(host, plain[index], agreement);
        hits += host.hit_distance > 0.0F ? 1U : 0U;
        exits += host.confidence == 0.0F ? 1U : 0U;
    }
    print("screen tier", agreement);
    // THE SKIP CHANGES NO ANSWER: the accelerated march against the plain one, bit for bit.
    u32 changed = 0;
    for (usize index = 0; index < rays.size(); ++index) {
        changed += (plain[index].confidence != accelerated[index].confidence ||
                    plain[index].hit_distance != accelerated[index].hit_distance ||
                    plain[index].radiance.x != accelerated[index].radiance.x)
                       ? 1U
                       : 0U;
    }
    std::fprintf(stderr,
                 "  %u hits, %u with no confidence; the pyramid changed %u answers and read %u "
                 "texels where the plain march read %u (%.2fx)\n",
                 hits, exits, changed, accelerated_counters.depth_reads, plain_counters.depth_reads,
                 static_cast<double>(plain_counters.depth_reads) /
                     std::max(1.0, static_cast<double>(accelerated_counters.depth_reads)));
    CY_CHECK_GT(hits, static_cast<u32>(rays.size() / 4));
    CY_CHECK_LE(agreement.disagreements, static_cast<u32>(rays.size() / 200));
    CY_CHECK_EQ(changed, 0U);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

namespace {

/// A floor to the horizon under an open sky, and a wall far down it: the view in which a gather
/// ray crosses long stretches of screen in front of everything — where a march spends its reads.
struct OpenFloor {
    std::vector<Vec3> positions{{-200.0F, 0.0F, -200.0F}, {200.0F, 0.0F, -200.0F},
                                {200.0F, 0.0F, 200.0F},   {-200.0F, 0.0F, 200.0F},
                                {-200.0F, 0.0F, -60.0F},  {200.0F, 0.0F, -60.0F},
                                {200.0F, 30.0F, -60.0F},  {-200.0F, 30.0F, -60.0F}};
    std::vector<u32> indices{0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    rt::AccelerationService service{active_service()};

    OpenFloor() {
        rt::TriangleGeometry triangles;
        triangles.positions = {positions.data(), positions.size()};
        triangles.indices = {indices.data(), indices.size()};
        CY_REQUIRE(service.declare_geometry(1, rt::adapt_static_mesh(triangles, 0)).has_value());
        rt::InstanceDescriptor instance;
        instance.geometry = 1;
        instance.instance_id = 1;
        CY_REQUIRE(service.add_instance(instance).has_value());
        (void)service.update();
    }
};

}  // namespace

CY_TEST_CASE("over open ground the pyramid strides where the plain march steps") {
    Gpu gpu;
    if (!gpu.available()) {
        return;
    }
    FrozenRoom room;
    OpenFloor open;
    const ScreenFixture screen(open.service, nullptr, 256, 144, Vec3{0.0F, 1.6F, 0.0F}, 0.12F);
    gi::ScreenView view = screen.view();
    view.max_steps = 64;
    TierRig rig(gpu.device(), room, false);
    GpuTierTracer tiers(rig.cache);
    CY_REQUIRE(tiers.set_view(&view).has_value());
    const std::vector<GpuTierRay> rays = screen.rays(2, 30.0F);
    std::vector<gi::RadianceSample> plain;
    std::vector<gi::RadianceSample> accelerated;
    GpuTierCounters plain_counters;
    GpuTierCounters accelerated_counters;
    CY_REQUIRE(run_tiers(gpu.device(), tiers, rays, GpuTierMode::Screen, plain, plain_counters));
    CY_REQUIRE(run_tiers(gpu.device(), tiers, rays, GpuTierMode::ScreenAccelerated, accelerated,
                         accelerated_counters));
    TierAgreement agreement;
    u32 changed = 0;
    for (usize index = 0; index < rays.size(); ++index) {
        const gi::RadianceSample host = gi::ScreenTracer::trace(
            view, rays[index].origin, rays[index].direction, rays[index].max_distance);
        (void)same_answer(host, plain[index], agreement);
        changed += (plain[index].confidence != accelerated[index].confidence ||
                    plain[index].hit_distance != accelerated[index].hit_distance ||
                    plain[index].radiance.x != accelerated[index].radiance.x)
                       ? 1U
                       : 0U;
    }
    print("screen tier, open ground", agreement);
    std::fprintf(
        stderr,
        "  the pyramid changed %u answers and read %u texels where the plain march read %u "
        "(%.2fx fewer)\n",
        changed, accelerated_counters.depth_reads, plain_counters.depth_reads,
        static_cast<double>(plain_counters.depth_reads) /
            std::max(1.0, static_cast<double>(accelerated_counters.depth_reads)));
    CY_CHECK_LE(agreement.disagreements, static_cast<u32>(rays.size() / 200));
    CY_CHECK_EQ(changed, 0U);
    // Where there is open screen to cross, the strides must pay for their tests — at least half.
    CY_CHECK_LT(static_cast<f64>(accelerated_counters.depth_reads),
                static_cast<f64>(plain_counters.depth_reads) * 0.5);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the world tiers answer as SoftwareTracer and HardwareTracer do, through the cache") {
    Gpu gpu;
    if (!gpu.available()) {
        return;
    }
    FrozenRoom room;
    const ScreenFixture screen(room, 128, 72);
    const std::vector<GpuTierRay> rays = screen.rays(1, 12.0F);
    {
        TierRig rig(gpu.device(), room, false);
        GpuTierTracer tiers(rig.cache);
        gi::TieredTracer::Config config;
        tiers.set_config(config);
        tiers.set_cache_frame(kCacheFrame);
        std::vector<gi::RadianceSample> device;
        GpuTierCounters counters;
        CY_REQUIRE(run_tiers(gpu.device(), tiers, rays, GpuTierMode::World, device, counters));
        TierAgreement agreement;
        for (usize index = 0; index < rays.size(); ++index) {
            const Vec3 unit = normalized_or(rays[index].direction, Vec3{0.0F, 0.0F, -1.0F});
            gi::SceneHit hit;
            (void)room.software.trace(rays[index].origin, unit, rays[index].max_distance, hit);
            (void)same_answer(room.software.radiance(hit, config.sky, unit), device[index],
                              agreement);
        }
        print("software tier", agreement);
        CY_CHECK_GT(agreement.rays, 1000U);
        CY_CHECK_LE(agreement.disagreements, agreement.rays / 100U);
    }
    if (!gpu.ray_queries()) {
        return;
    }
    TierRig rig(gpu.device(), room, true);
    GpuTierTracer tiers(rig.cache);
    gi::TieredTracer::Config config;
    tiers.set_config(config);
    tiers.set_cache_frame(kCacheFrame);
    std::vector<gi::RadianceSample> device;
    GpuTierCounters counters;
    CY_REQUIRE(run_tiers(gpu.device(), tiers, rays, GpuTierMode::World, device, counters));
    TierAgreement agreement;
    for (usize index = 0; index < rays.size(); ++index) {
        const Vec3 unit = normalized_or(rays[index].direction, Vec3{0.0F, 0.0F, -1.0F});
        gi::SceneHit hit;
        (void)room.hardware.trace(rays[index].origin, unit, rays[index].max_distance, hit);
        (void)same_answer(room.hardware.radiance(hit, config.sky, unit), device[index], agreement);
    }
    print("hardware tier", agreement);
    CY_CHECK_LE(agreement.disagreements, agreement.rays / 100U);
    room.service.set_mirror(nullptr);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the software tier's confidence follows the field's own resolution") {
    // REGRESSION. `SoftwareTracer` measures a grazing ray against four of the field's finest
    // voxels, and the device's scale was a constant 1.0 — which is exactly four quarter-metre
    // voxels, the only field every other case here uses. A half-metre field grazes at two metres.
    Gpu gpu;
    if (!gpu.available()) {
        return;
    }
    gi::ClipmapSettings coarse = gi_support::room_settings().field;
    coarse.resolution = 32;
    FrozenRoom room(coarse);
    const ScreenFixture screen(room, 128, 72);
    const std::vector<GpuTierRay> rays = screen.rays(1, 12.0F);
    TierRig rig(gpu.device(), room, false);
    GpuTierTracer tiers(rig.cache);
    gi::TieredTracer::Config config;
    tiers.set_config(config);
    tiers.set_cache_frame(kCacheFrame);
    std::vector<gi::RadianceSample> device;
    GpuTierCounters counters;
    CY_REQUIRE(run_tiers(gpu.device(), tiers, rays, GpuTierMode::World, device, counters));
    TierAgreement agreement;
    for (usize index = 0; index < rays.size(); ++index) {
        const Vec3 unit = normalized_or(rays[index].direction, Vec3{0.0F, 0.0F, -1.0F});
        gi::SceneHit hit;
        (void)room.software.trace(rays[index].origin, unit, rays[index].max_distance, hit);
        (void)same_answer(room.software.radiance(hit, config.sky, unit), device[index], agreement);
    }
    print("software tier, half-metre field", agreement);
    CY_CHECK_GT(agreement.rays, 1000U);
    CY_CHECK_LE(agreement.disagreements, agreement.rays / 100U);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("an escalation nobody configured answers as a default TieredTracer does") {
    // REGRESSION. The device tiers' configuration, budget and sky were zero until a caller set
    // them: a batch declared without `set_config` and `set_budget` reached nothing — every world
    // ray had zero reach and zero budget — where a default `TieredTracer` and `TraceBudget` trace
    // sixty metres, sixty-four rays, under the default sky.
    Gpu gpu;
    if (!gpu.available()) {
        return;
    }
    FrozenRoom room;
    const ScreenFixture screen(room, 256, 144);
    const gi::ScreenView view = screen.view();
    const std::vector<GpuTierRay> rays = screen.rays(4, 8.0F);
    TierRig rig(gpu.device(), room, false);
    GpuTierTracer tiers(rig.cache);
    tiers.set_cache_frame(kCacheFrame);
    CY_REQUIRE(tiers.set_view(&view).has_value());
    std::vector<gi::RadianceSample> device;
    GpuTierCounters counters;
    CY_REQUIRE(run_tiers(gpu.device(), tiers, rays, GpuTierMode::Tiered, device, counters));
    gi::TieredTracer host;
    host.set_screen(&view);
    host.set_software(&room.software);
    gi::TraceBudget budget;
    for (const GpuTierRay& ray : rays) {
        (void)host.trace(ray.origin, ray.direction, ray.max_distance, budget);
    }
    std::fprintf(stderr, "  default escalation: world rays %u/%u, refused %u/%u (host/device)\n",
                 host.diagnostics().software_rays, counters.world_rays,
                 host.diagnostics().budget_refusals, counters.budget_refusals);
    CY_CHECK_GT(host.diagnostics().software_rays, 0U);
    CY_CHECK_EQ(counters.world_rays, host.diagnostics().software_rays);
    CY_CHECK_EQ(counters.budget_refusals, host.diagnostics().budget_refusals);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the escalation answers as TieredTracer does, and a spent budget refuses as it does") {
    Gpu gpu;
    if (!gpu.available()) {
        return;
    }
    FrozenRoom room;
    const ScreenFixture screen(room, 256, 144);
    const gi::ScreenView view = screen.view();
    const std::vector<GpuTierRay> rays = screen.rays(2, 8.0F);
    const bool hardware = gpu.ray_queries();
    for (const bool use_hardware : {false, hardware}) {
        TierRig rig(gpu.device(), room, use_hardware);
        GpuTierTracer tiers(rig.cache);
        gi::TieredTracer::Config config;
        tiers.set_config(config);
        tiers.set_cache_frame(kCacheFrame);
        CY_REQUIRE(tiers.set_view(&view).has_value());

        gi::TieredTracer host;
        host.set_config(config);
        host.set_screen(&view);
        host.set_software(&room.software);
        if (use_hardware) {
            host.set_hardware(&room.hardware);
        }
        // An unbounded budget first: every answer comparable, ray for ray.
        gi::TraceBudget plenty;
        plenty.world_rays = static_cast<u32>(rays.size());
        plenty.escalations = static_cast<u32>(rays.size());
        tiers.set_budget(plenty);
        std::vector<gi::RadianceSample> device;
        GpuTierCounters counters;
        CY_REQUIRE(run_tiers(gpu.device(), tiers, rays, GpuTierMode::Tiered, device, counters));
        TierAgreement agreement;
        gi::TraceBudget host_budget = plenty;
        for (usize index = 0; index < rays.size(); ++index) {
            const gi::RadianceSample answer = host.trace(rays[index].origin, rays[index].direction,
                                                         rays[index].max_distance, host_budget);
            (void)same_answer(answer, device[index], agreement);
        }
        const gi::TracingDiagnostics& expected = host.diagnostics();
        print(use_hardware ? "escalation, hardware world tier" : "escalation, software world tier",
              agreement);
        std::fprintf(stderr, "    screen %u/%u, escalations %u/%u, world %u/%u (host/device)\n",
                     expected.screen_rays, counters.screen_rays, expected.escalations,
                     counters.escalations, expected.software_rays + expected.hardware_rays,
                     counters.world_rays);
        CY_CHECK_LE(agreement.disagreements, agreement.rays / 100U);
        CY_CHECK_EQ(counters.screen_rays, expected.screen_rays);
        CY_CHECK_EQ(counters.escalations, expected.escalations);
        CY_CHECK_EQ(counters.world_rays, expected.software_rays + expected.hardware_rays);
        CY_CHECK_EQ(counters.hardware_rays, expected.hardware_rays);
        CY_CHECK_EQ(counters.software_rays, expected.software_rays);
        CY_CHECK_EQ(counters.budget_refusals, 0U);
        CY_CHECK_GT(counters.escalations, 0U);
        CY_CHECK_LT(counters.escalations, counters.screen_rays);

        // A budget a third of what the batch wants: as many refused as the host refuses, and as
        // many issued. WHICH rays are refused is the order threads reach the counter, and is not
        // compared.
        gi::TraceBudget tight;
        tight.world_rays = expected.escalations / 3U;
        tight.escalations = expected.escalations / 3U;
        tiers.set_budget(tight);
        host.reset_diagnostics();
        gi::TraceBudget host_tight = tight;
        for (const GpuTierRay& ray : rays) {
            (void)host.trace(ray.origin, ray.direction, ray.max_distance, host_tight);
        }
        CY_REQUIRE(run_tiers(gpu.device(), tiers, rays, GpuTierMode::Tiered, device, counters));
        std::fprintf(stderr, "    budget %u: refused %u/%u, issued %u/%u (host/device)\n",
                     tight.world_rays, host.diagnostics().budget_refusals, counters.budget_refusals,
                     host.diagnostics().escalations, counters.world_rays);
        CY_CHECK_EQ(counters.budget_refusals, host.diagnostics().budget_refusals);
        CY_CHECK_EQ(counters.world_rays, tight.world_rays);
        CY_CHECK_EQ(counters.escalations, host.diagnostics().escalations);
        room.service.set_mirror(nullptr);
        if (!hardware) {
            break;
        }
    }
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

// --- The cost, and the picture -------------------------------------------------------------------

CY_TEST_CASE("what stage 3 costs on this device: the gather at its budget and the tiers at 1080p") {
    Gpu gpu;
    if (!gpu.available()) {
        return;
    }
    if (!gpu.device().capabilities().has(rhi::Capability::TimestampQueries)) {
        std::fprintf(stderr, "this device has no timestamp queries; nothing to time\n");
        return;
    }
    FrozenRoom room;
    const bool hardware = gpu.ray_queries();

    // THE GATHER at the GI budget's default rate: 64 probes a frame, 32 rays a probe.
    for (const bool use_hardware : {false, hardware}) {
        ProbePair pair(gpu.device(), room, gi::ProbeEncoding::SphericalHarmonicsL1, use_hardware);
        pair.gpu.set_timing(true);
        gi::ProbeUpdateContext context = pair.context(1);
        context.converged = false;
        context.budget = 64;
        context.rays_per_probe = 32;
        f64 best = 1.0e9;
        for (u64 frame = 1; frame <= 5; ++frame) {
            context.frame = frame;
            const gi::ProbeUpdateReport report = pair.device.update(context);
            CY_REQUIRE_EQ(report.probes_updated, 64U);
            CY_REQUIRE(run_frame(gpu.device(), [&](rendering::RenderGraph& graph) {
                return pair.gpu.declare(graph);
            }));
            (void)pair.device.collect();
            const GpuDispatchTime time = pair.gpu.last_time();
            CY_REQUIRE(time.valid);
            best = std::min(best, time.milliseconds);
        }
        std::fprintf(stderr, "  cost: gather, 64 probes x 32 rays, %s tier: %.3f ms\n",
                     use_hardware ? "hardware" : "software", best);
        CY_CHECK_GT(best, 0.0);
        if (!hardware) {
            break;
        }
    }

    // THE TIERS AT 1080p: one gather ray per pixel, through the screen tier alone with and without
    // the pyramid, and through the escalation with each world tier — in the closed room, where a
    // gather ray meets a wall within a few steps, and over open ground, where it crosses the
    // screen.
    constexpr u32 kWidth = 1920;
    constexpr u32 kHeight = 1080;
    GpuRadianceCacheDescription description;
    description.max_tier_rays = kWidth * kHeight;
    description.max_screen_pixels = kWidth * kHeight;
    struct Timing {
        const char* what;
        GpuTierMode mode;
        bool hardware;
    };
    const Timing timings[] = {
        {"screen tier", GpuTierMode::Screen, false},
        {"screen tier, pyramid", GpuTierMode::ScreenAccelerated, false},
        {"escalation, software world tier", GpuTierMode::Tiered, false},
        {"escalation, hardware world tier", GpuTierMode::Tiered, true},
    };
    OpenFloor open;
    for (const bool outdoors : {false, true}) {
        constexpr u32 kFactor = 4;
        const ScreenFixture screen =
            outdoors ? ScreenFixture(open.service, nullptr, kWidth / kFactor, kHeight / kFactor,
                                     Vec3{0.0F, 1.6F, 0.0F}, 0.12F)
                           .upscaled(kFactor)
                     : ScreenFixture(room, kWidth / kFactor, kHeight / kFactor).upscaled(kFactor);
        gi::ScreenView view = screen.view();
        view.max_steps = 64;
        const std::vector<GpuTierRay> rays = screen.rays(1, outdoors ? 30.0F : 8.0F);
        u32 plain_reads = 0;
        u32 accelerated_reads = 0;
        for (const Timing& timing : timings) {
            if (timing.hardware && !hardware) {
                continue;
            }
            TierRig rig(gpu.device(), room, timing.hardware, description);
            GpuTierTracer tiers(rig.cache);
            gi::TieredTracer::Config config;
            tiers.set_config(config);
            tiers.set_cache_frame(kCacheFrame);
            gi::TraceBudget budget;
            budget.world_rays = static_cast<u32>(rays.size());
            budget.escalations = static_cast<u32>(rays.size());
            tiers.set_budget(budget);
            CY_REQUIRE(tiers.set_view(&view).has_value());
            rig.cache.set_timing(true);
            // The rays once and the counters only: what is timed is the device, and the host's
            // share of a 1080p batch — two million rays written and read — is the case's budget.
            CY_REQUIRE(tiers.set_rays({rays.data(), rays.size()}).has_value());
            GpuTierCounters counters;
            f64 best = 1.0e9;
            for (u32 repeat = 0; repeat < 3; ++repeat) {
                CY_REQUIRE(run_frame(gpu.device(), [&](rendering::RenderGraph& graph) {
                    return tiers.declare(graph, timing.mode);
                }));
                CY_REQUIRE(tiers.read_back({}, counters).has_value());
                const GpuDispatchTime time = rig.cache.last_time();
                CY_REQUIRE(time.valid);
                best = std::min(best, time.milliseconds);
            }
            std::fprintf(stderr,
                         "  cost: %s, %s, %zu rays at %ux%u: %.3f ms (%u escalated, %u depth "
                         "texels read)\n",
                         outdoors ? "open ground" : "closed room", timing.what, rays.size(), kWidth,
                         kHeight, best, counters.escalations, counters.depth_reads);
            CY_CHECK_GT(best, 0.0);
            if (timing.mode == GpuTierMode::Screen) {
                plain_reads = counters.depth_reads;
            } else if (timing.mode == GpuTierMode::ScreenAccelerated) {
                accelerated_reads = counters.depth_reads;
            }
            room.service.set_mirror(nullptr);
        }
        if (outdoors) {
            // The pyramid's claim at the resolution it is for, where there is screen to cross.
            CY_CHECK_LT(static_cast<f64>(accelerated_reads), static_cast<f64>(plain_reads) * 0.5);
        }
    }
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

namespace {

/// The floor's resolved indirect diffuse, top down, tone mapped, into one panel of `texels`.
void paint_floor(const gi::IlluminationSystem& system, u32 size, u32 x_offset, u32 stride,
                 std::vector<u32>& texels) {
    const gi::SurfaceProperties surface;
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const f32 wx =
                ((((static_cast<f32>(x) + 0.5F) / static_cast<f32>(size)) * 2.0F) - 1.0F) *
                (gi_support::kRoomX - 0.05F);
            const f32 wz =
                ((((static_cast<f32>(y) + 0.5F) / static_cast<f32>(size)) * 2.0F) - 1.0F) *
                (gi_support::kRoomZ - 0.05F);
            const Vec3 radiance = system
                                      .indirect_diffuse(Vec3{wx, -gi_support::kRoomY + 0.05F, wz},
                                                        Vec3{0.0F, 1.0F, 0.0F}, surface)
                                      .radiance *
                                  0.3F;
            const auto channel = [](f32 value) {
                const f32 mapped = std::pow(value / (1.0F + value), 1.0F / 2.2F);
                return static_cast<u32>(std::lround(std::clamp(mapped, 0.0F, 1.0F) * 255.0F));
            };
            texels[(static_cast<usize>(y) * stride) + x_offset + x] =
                channel(radiance.x) | (channel(radiance.y) << 8U) | (channel(radiance.z) << 16U) |
                0xFF000000U;
        }
    }
}

}  // namespace

CY_TEST_CASE(
    "the floor's indirect light: host gather, device software tier, device hardware tier") {
    Gpu gpu;
    if (!gpu.available()) {
        return;
    }
    const bool hardware = gpu.ray_queries();
    DeviceRoom host(nullptr, false);
    DeviceRoom software(&gpu.device(), false);
    DeviceRoom hardware_room(&gpu.device(), hardware);
    CY_REQUIRE(host.run(0, kConvergedFrames));
    CY_REQUIRE(software.run(0, kConvergedFrames));
    CY_REQUIRE(hardware_room.run(0, kConvergedFrames));

    constexpr u32 kSize = 256;
    constexpr u32 kGap = 8;
    constexpr u32 kStride = (kSize * 3U) + (kGap * 2U);
    std::vector<u32> texels(static_cast<usize>(kStride) * kSize, 0xFF201A18U);
    paint_floor(host.system, kSize, 0, kStride, texels);
    paint_floor(software.system, kSize, kSize + kGap, kStride, texels);
    paint_floor(hardware_room.system, kSize, (kSize + kGap) * 2U, kStride, texels);

    // The host and the device software tier are one picture: texels more than two steps apart.
    u32 differing = 0;
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            const u32 a = texels[(static_cast<usize>(y) * kStride) + x];
            const u32 b = texels[(static_cast<usize>(y) * kStride) + kSize + kGap + x];
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
    std::fprintf(stderr,
                 "floor: host vs device software tier, %u of %u texels differ by more than "
                 "two steps\n",
                 differing, kSize * kSize);
    CY_CHECK_LE(differing, kSize * kSize / 100U);
    render_test::Image image(allocator());
    CY_REQUIRE(
        render_test::adopt(image, {texels.data(), texels.size()}, kStride, kSize).has_value());
    const char* path = CY_TEST_BINARY_DIR "/gi-gpu-radiance-cache.png";
    CY_REQUIRE(render_test::write_png(path, image).has_value());
    std::fprintf(stderr, "wrote %s (host gather, device software tier, device %s tier)\n", path,
                 hardware ? "hardware" : "software");
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the set layouts the C++ creates are the ones the compiled entry points declare") {
    // Reflected from the committed SPIR-V, so a field added to `GiProbeSet` or `GiHardwareSet`
    // without the matching binding here — or the other way round — fails without a device. The
    // ray query's top level is read as an acceleration structure: `rhi::DescriptorKind` has one
    // since this stage, and the reflection refused the type by name before it.
    struct Entry {
        const char* name;
        Span<const u32> words;
        bool hardware;
    };
    namespace shaders = cy::rendering::gi_gpu;
    const Entry entries[] = {
        {"cyGiGatherProbes",
         {shaders::kGiGatherProbesSpirv, std::size(shaders::kGiGatherProbesSpirv)},
         false},
        {"cyGiTraceTiered",
         {shaders::kGiTraceTieredSpirv, std::size(shaders::kGiTraceTieredSpirv)},
         false},
        {"cyGiGatherProbesHardware",
         {shaders::kGiGatherProbesHardwareSpirv, std::size(shaders::kGiGatherProbesHardwareSpirv)},
         true},
        {"cyGiTraceTieredHardware",
         {shaders::kGiTraceTieredHardwareSpirv, std::size(shaders::kGiTraceTieredHardwareSpirv)},
         true},
    };
    for (const Entry& entry : entries) {
        auto reflection = cy::shader::reflect_spirv(allocator(), entry.words);
        CY_REQUIRE(reflection.has_value());
        bool saw_top_level = false;
        for (const cy::shader::ReflectedBinding& binding : reflection->bindings()) {
            if (binding.set == 2) {
                // `GiHardwareSet`: the top level, then three storage buffers.
                CY_CHECK(binding.binding < 4U);
                CY_CHECK_EQ(binding.kind, binding.binding == 0
                                              ? rhi::DescriptorKind::AccelerationStructure
                                              : rhi::DescriptorKind::StorageBuffer);
                saw_top_level = saw_top_level || binding.binding == 0;
            } else {
                // `GiSceneSet` (13 bindings) and `GiProbeSet` (15): storage buffers only.
                CY_CHECK(binding.set < 2U);
                CY_CHECK(binding.binding < (binding.set == 0 ? 13U : 15U));
                CY_CHECK_EQ(binding.kind, rhi::DescriptorKind::StorageBuffer);
            }
        }
        std::fprintf(stderr, "  %s: %zu bindings%s\n", entry.name, reflection->bindings().size(),
                     saw_top_level ? ", the top level at (2, 0)" : "");
        CY_CHECK_EQ(saw_top_level, entry.hardware);
    }
}
