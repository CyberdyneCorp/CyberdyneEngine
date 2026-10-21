// SPDX-License-Identifier: MIT
// The GI streamed with world cells, on the device. Issue #35, the world half.
//
// A strip of 16 m cells — a floor slab, a building, the meshes' distance fields — streamed in ahead
// of a traveller and out behind through `world::CellEventQueue`, as `world::WorldStreaming` emits
// them, consumed by `gi_streaming::GiCellStreaming`; the caches' arithmetic on the device
// (`gi_gpu::GpuGiCaches`): every frame the host's update, then the card shading and the probe
// gather in a device frame of their own. Asserted:
//
//   incremental   a frame uploads the cards that arrived, left or were invalidated, never the
//                 cache; the lookup grid lists again only the cards that arrived or left, and is
//                 never rebuilt whole after the first frame; the field uploads the bricks a
//                 placement touched
//   no hitch      the host's frame — the streaming tick, the update and the submission — and the
//                 device's card shading and gather, reported per frame, the host's held below a
//                 60 Hz frame; the deterministic half — what a tick may ingest — is the budget's
//
// That the streamed GI is the GI that never streamed is `smoke.render_gi_streaming`'s, on the host
// oracle; that the device shades a card as the host does is `render.gi_gpu`'s.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi/mesh_cards.h>
#include <cy/rendering/gi/system.h>
#include <cy/rendering/gi_gpu/gpu_caches.h>
#include <cy/rendering/gi_gpu/gpu_radiance_cache.h>
#include <cy/rendering/gi_gpu/gpu_scene.h>
#include <cy/rendering/gi_streaming/gi_streaming.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/test/test.h>
#include <cy/world/activation.h>
#include <cy/world/partition.h>

#include "device.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <vector>

using namespace cy;
namespace gi = cy::rendering::gi;
namespace gi_gpu = cy::rendering::gi_gpu;
namespace gs = cy::rendering::gi_streaming;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

constexpr f32 kCellMetres = 16.0F;
constexpr i32 kCells = 6;
constexpr u32 kFramesPerCell = 12;
/// A hitch is a frame that misses a 60 Hz deadline. The host's whole GI frame — the streaming tick,
/// the update and the device submission — is held below it: measured at 0.45 ms median and 2.5 ms
/// worst on an idle RTX 5060 host, and the deadline leaves room for a build running beside it,
/// which put the same frames at 5 ms.
constexpr f64 kHitchThresholdMs = 1000.0 / 60.0;

[[nodiscard]] f64 cpu_now_ms() noexcept {
#if defined(CLOCK_THREAD_CPUTIME_ID)
    timespec now{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0) {
        return (static_cast<f64>(now.tv_sec) * 1e3) + (static_cast<f64>(now.tv_nsec) * 1e-6);
    }
#endif
    return std::chrono::duration<f64, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void box_mesh(Vec3 half, std::vector<Vec3>& positions, std::vector<u32>& indices) {
    positions.clear();
    for (u32 corner = 0; corner < 8; ++corner) {
        positions.push_back(Vec3{(corner & 1U) != 0U ? half.x : -half.x,
                                 (corner & 2U) != 0U ? half.y : -half.y,
                                 (corner & 4U) != 0U ? half.z : -half.z});
    }
    indices = {0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1,
               2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3};
}

struct Piece {
    std::vector<Vec3> positions;
    std::vector<u32> indices;
    gi::MeshDistanceField field;
    gi::AssetDistanceField asset{};

    explicit Piece(Vec3 half) {
        box_mesh(half, positions, indices);
        gi::MeshFieldSettings settings;
        settings.dimension = 16;
        CY_REQUIRE(field
                       .build({positions.data(), positions.size()},
                              {indices.data(), indices.size()}, settings)
                       .has_value());
        asset = field.asset();
    }
};

/// The strip: cell `x` along +x, each a floor and a building, cooked once.
struct Strip {
    Piece floor{Vec3{kCellMetres * 0.5F, 0.1F, kCellMetres * 0.5F}};
    Piece building{Vec3{2.0F, 1.5F, 2.0F}};
    world::PartitionConfig partition = world::uniform_grid_config(kCellMetres);
    std::vector<std::unique_ptr<gs::GiCellContent>> cooked;
    std::vector<world::CellId> ids;

    Strip() {
        for (i32 x = 0; x < kCells; ++x) {
            ids.push_back(world::cell_id_of(partition, world::CellCoord{x, 0, 0, 0}));
            auto content = std::make_unique<gs::GiCellContent>(allocator());
            const Vec3 middle{(static_cast<f32>(x) + 0.5F) * kCellMetres, 0.0F, 8.0F};
            const Mat4 at_floor = Mat4::from_translation(middle + Vec3{0.0F, -0.1F, 0.0F});
            const Mat4 at_building = Mat4::from_translation(middle + Vec3{2.0F, 1.5F, 0.0F});
            gi::MeshCardSettings carding;
            carding.spacing_metres = 1.0F;
            carding.instance_id = static_cast<u32>(x * 2) + 1U;
            CY_REQUIRE(gi::append_mesh_cards({floor.positions.data(), floor.positions.size()},
                                             {floor.indices.data(), floor.indices.size()}, at_floor,
                                             carding, content->cards)
                           .has_value());
            carding.spacing_metres = 0.5F;
            carding.albedo = x % 2 == 0 ? Vec3{0.7F, 0.2F, 0.2F} : Vec3{0.2F, 0.6F, 0.3F};
            carding.instance_id = static_cast<u32>(x * 2) + 2U;
            CY_REQUIRE(gi::append_mesh_cards({building.positions.data(), building.positions.size()},
                                             {building.indices.data(), building.indices.size()},
                                             at_building, carding, content->cards)
                           .has_value());
            CY_REQUIRE(content->fields.push_back(gs::GiFieldPlacement{&floor.asset, at_floor})
                           .has_value());
            CY_REQUIRE(content->fields.push_back(gs::GiFieldPlacement{&building.asset, at_building})
                           .has_value());
            cooked.push_back(std::move(content));
        }
    }
};

Status load(void* user, world::CellId cell, const world::PartitionConfig& /*partition*/,
            gs::GiCellContent& out) noexcept {
    const auto* strip = static_cast<const Strip*>(user);
    for (usize index = 0; index < strip->ids.size(); ++index) {
        if (strip->ids[index] == cell) {
            const gs::GiCellContent& cooked = *strip->cooked[index];
            if (Status copied = out.cards.append(cooked.cards.span()); !copied) {
                return copied;
            }
            return out.fields.append(cooked.fields.span());
        }
    }
    return ok();
}

[[nodiscard]] world::CellEvent event(world::CellId cell, world::CellEventKind kind,
                                     u64 sequence) noexcept {
    world::CellEvent out;
    out.kind = kind;
    out.cell = cell;
    out.channels = world::ChannelMask::all();
    out.sequence = sequence;
    return out;
}

[[nodiscard]] gi::IlluminationSettings settings() noexcept {
    gi::IlluminationSettings out;
    out.field.levels = 2;
    out.field.resolution = 32;
    out.field.base_extent_metres = 16.0F;
    out.probes.levels = 2;
    out.probes.half_extent_probes = 4;
    out.probes.base_spacing_metres = 2.0F;
    out.rays_per_probe = 16;
    out.max_ray_distance_metres = 24.0F;
    out.surface_bounce = gi::SurfaceBounce::Cards;
    out.card_gather.rays = 8;
    out.sky.zenith = Vec3{0.4F, 0.5F, 0.7F};
    out.sky.horizon = Vec3{0.6F, 0.6F, 0.6F};
    return out;
}

}  // namespace

CY_TEST_CASE("cells streamed in and out reach the device incrementally, with no hitch") {
    render_test::DeviceFixture fixture{"vulkan", "smoke.gi_gpu_streaming"};
    if (!fixture.is(rhi::BackendKind::Vulkan) || !gi_gpu::GpuGiScene::supported(fixture.device())) {
        std::fprintf(stderr, "no requested graphics device on this machine; ");
        fixture.report_skip();
        return;
    }
    rhi::Device& device = fixture.device();
    Strip strip;

    gi::IlluminationSystem system;
    CY_REQUIRE(system.configure(settings()).has_value());
    system.surfaces().set_lookup_radius(0.75F);
    gi_gpu::GpuGiScene scene;
    gi_gpu::GpuGiSceneDescription scene_description;
    scene_description.max_levels = 2;
    scene_description.max_window_bricks = 8;
    scene_description.max_brick_slots = 8192;
    scene_description.max_cards = 8192;
    scene_description.max_selection = 1024;
    CY_REQUIRE(scene.create(allocator(), device, scene_description).has_value());
    gi_gpu::GpuRadianceCache radiance;
    gi_gpu::GpuRadianceCacheDescription cache_description;
    cache_description.max_probes = 2 * 9 * 9 * 9;
    CY_REQUIRE(radiance.create(allocator(), device, scene, cache_description).has_value());
    radiance.bind_field(system.field());
    radiance.bind_host_surfaces(nullptr);
    gi_gpu::GpuGiCaches caches;
    CY_REQUIRE(caches.create(device, scene, radiance, system).has_value());

    world::CellEventQueue events(allocator());
    gs::GiCellStreaming streaming(allocator(), system, strip.partition);
    CY_REQUIRE(streaming.attach(events, 50).has_value());
    streaming.set_loader(&load, &strip);
    gs::GiStreamingBudget budget;
    budget.cards_per_tick = 512;
    budget.fields_per_tick = 1;

    gi::GiLight sun;
    sun.directional = true;
    sun.direction = normalize(Vec3{0.3F, -1.0F, 0.2F});
    sun.intensity = 3.0F;
    sun.id = 1;

    // The traveller walks the strip; the two cells about it are active.
    u64 sequence = 1;
    std::vector<bool> active(kCells, false);
    std::vector<f64> host_ms;
    std::vector<f64> device_ms;
    u32 whole_builds = 0;
    u32 incremental_frames = 0;
    u32 over_upload = 0;
    u32 over_budget = 0;
    u32 gathered_frames = 0;
    u32 shaded_frames = 0;
    u64 frame = 0;
    const u32 frames = static_cast<u32>(kCells) * kFramesPerCell;
    for (u32 step = 0; step < frames; ++step) {
        const f32 travelled = (static_cast<f32>(step) + 0.5F) / static_cast<f32>(kFramesPerCell);
        const i32 here = std::min(kCells - 1, static_cast<i32>(travelled));
        for (i32 x = 0; x < kCells; ++x) {
            const bool wanted = x == here || x == here + 1;
            if (wanted != active[x]) {
                CY_REQUIRE(events
                               .emit(event(strip.ids[x],
                                           wanted ? world::CellEventKind::Activated
                                                  : world::CellEventKind::Deactivated,
                                           sequence++))
                               .has_value());
                active[x] = wanted;
            }
        }
        ++frame;
        const u64 cards_before = streaming.cards_in_gi();
        const f64 started = cpu_now_ms();
        const auto streamed = streaming.tick(frame, budget);
        CY_REQUIRE(streamed.has_value());
        // What arrived fits what the tick had left after what left — or is one instance larger
        // than the whole budget (the floor slab's 576 cards), alone in a tick that evicted nothing.
        // What left is counted from the cards the GI holds, not from the report.
        const auto released =
            static_cast<u32>(cards_before + streamed->cards_ingested - streaming.cards_in_gi());
        const u32 room = budget.cards_per_tick - std::min(budget.cards_per_tick, released);
        over_budget +=
            streamed->cards_ingested > room && (streamed->chunks_ingested != 1U || released != 0U)
                ? 1U
                : 0U;
        gi::FrameContext context;
        context.camera = Vec3{travelled * kCellMetres, 1.6F, 8.0F};
        context.lights = {&sun, 1};
        context.frame = frame;
        const gi::IlluminationFrameReport report = system.update(context);
        host_ms.push_back(cpu_now_ms() - started);

        CY_REQUIRE(device.begin_frame().has_value());
        rendering::RenderGraph graph(allocator());
        CY_REQUIRE(caches.declare(graph).has_value());
        rendering::GraphExecutor executor(allocator(), device);
        CY_REQUIRE(executor.execute(graph, rendering::CompileOptions{}, rendering::ExecuteOptions{})
                       .has_value());
        CY_REQUIRE(device.wait_idle().has_value());
        CY_REQUIRE(device.end_frame().has_value());
        const gi_gpu::GpuGiCost cost = caches.cost();
        gathered_frames += cost.gather_valid ? 1U : 0U;
        shaded_frames += cost.cards_valid ? 1U : 0U;
        device_ms.push_back((cost.cards_valid ? cost.cards_ms : 0.0) +
                            (cost.gather_valid ? cost.gather_ms : 0.0));

        // INCREMENTAL: what reached the device this frame is what changed.
        const gi_gpu::CardUploadReport& cards = caches.shading().last_submission().cards;
        if (step > 0U) {
            // Once the working set is whole — two cells, after the first one's frames — a cell
            // arriving or leaving never builds the grid whole: the bucket count holds.
            whole_builds += cards.grid_built_whole && step >= 2U * kFramesPerCell ? 1U : 0U;
            incremental_frames += cards.grid_rebuilt && !cards.grid_built_whole ? 1U : 0U;
            // Arrived and left this frame, and the pages an invalidation touched.
            const auto left =
                static_cast<u32>(cards_before + streamed->cards_ingested - streaming.cards_in_gi());
            const u32 changed = streamed->cards_ingested + left + report.invalidated_surface_pages;
            over_upload += cards.cards_uploaded > changed ? 1U : 0U;
            CY_CHECK_LE(cards.cards_relisted, streamed->cards_ingested + left);
        }
    }
    CY_CHECK_EQ(over_upload, 0U);
    CY_CHECK_EQ(whole_builds, 0U);
    CY_CHECK_GT(incremental_frames, 4U);

    std::vector<f64> host_sorted = host_ms;
    std::ranges::sort(host_sorted);
    std::vector<f64> device_sorted = device_ms;
    std::ranges::sort(device_sorted);
    CY_REQUIRE(host_sorted.size() > 2U);
    CY_REQUIRE(!device_sorted.empty());
    const f64 host_median = host_sorted[host_sorted.size() / 2];
    const f64 host_worst = host_sorted[host_sorted.size() - 1U];
    const f64 host_second = host_sorted[host_sorted.size() - 2U];
    const f64 device_median = device_sorted[device_sorted.size() / 2];
    const f64 device_worst = device_sorted[device_sorted.size() - 1U];
    std::fprintf(stderr,
                 "streamed %d cells along the strip in %u frames: host frame median %.3f ms, worst "
                 "%.3f ms; device cards and gather median %.3f ms, worst %.3f ms; the grid listed "
                 "only the changed cards in %u frames\n",
                 kCells, frames, host_median, host_worst, device_median, device_worst,
                 incremental_frames);
    // The first frame places every probe and solves every brick: a load, not a stream.
    CY_CHECK_LT(host_second, kHitchThresholdMs * cy::test::kSanitizedAllowance);
    CY_CHECK_EQ(over_budget, 0U);

    CY_CHECK_GT(streaming.cards_in_gi(), 0U);
    // Both halves of the caches ran on the device, and the probes it gathered came back valid.
    CY_CHECK_GT(shaded_frames, frames / 2U);
    CY_CHECK_GT(gathered_frames, frames / 2U);
    u32 valid_probes = 0;
    for (const gi::Probe& probe : system.radiance().probes()) {
        valid_probes += probe.live && probe.valid ? 1U : 0U;
    }
    CY_CHECK_GT(valid_probes, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
    caches.destroy();
}
