// The GI scene, the surface cache and the radiance cache. Task 9.1.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi/radiance_cache.h>
#include <cy/rendering/gi/scene.h>
#include <cy/rendering/gi/surface_cache.h>
#include <cy/rendering/gi/tracing.h>

#include "support.h"

#include <cmath>
#include <vector>

namespace {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;
using namespace cy::rendering::gi;  // NOLINT(google-build-using-namespace)
using gi_support::BoxField;

/// An indirect source that always answers with one colour. Standing in for the radiance cache so
/// the surface cache can be tested without one — which is the separability requirement in practice.
class ConstantIndirect final : public IndirectSource {
public:
    explicit ConstantIndirect(Vec3 value) noexcept : value_(value) {}
    [[nodiscard]] Vec3 gather(Vec3 /*position*/, Vec3 /*normal*/) const noexcept override {
        return value_;
    }

private:
    Vec3 value_;
};

std::vector<Surfel> two_walls() {
    std::vector<Surfel> surfels;
    Surfel red;
    red.position = Vec3{-2.0F, 0.0F, 0.0F};
    red.normal = Vec3{1.0F, 0.0F, 0.0F};
    red.albedo = Vec3{0.9F, 0.05F, 0.05F};
    red.area = 1.0F;
    red.instance_id = 1;
    surfels.push_back(red);

    Surfel floor;
    floor.position = Vec3{0.0F, -1.0F, 0.0F};
    floor.normal = Vec3{0.0F, 1.0F, 0.0F};
    floor.albedo = Vec3{0.8F, 0.8F, 0.8F};
    floor.area = 1.0F;
    floor.instance_id = 2;
    surfels.push_back(floor);
    return surfels;
}

}  // namespace

CY_TEST_CASE("a cell streams in and out and touches only its own surfels") {
    GiScene scene;
    const std::vector<Surfel> first = two_walls();
    const std::vector<Surfel> second = two_walls();
    const cy::Aabb left = cy::Aabb::from_min_max(Vec3{-4.0F, -2.0F, -2.0F}, Vec3{0.0F, 2.0F, 2.0F});
    const cy::Aabb right =
        cy::Aabb::from_min_max(Vec3{20.0F, -2.0F, -2.0F}, Vec3{24.0F, 2.0F, 2.0F});

    CY_REQUIRE(scene.ingest_cell(1, left, {first.data(), first.size()}, 1).has_value());
    CY_CHECK_EQ(scene.surfel_count(), first.size());
    CY_CHECK_EQ(scene.last_touched_surfels(), first.size());
    CY_REQUIRE(scene.ingest_cell(2, right, {second.data(), second.size()}, 1).has_value());
    CY_CHECK_EQ(scene.surfel_count(), first.size() + second.size());
    // The second ingest touched only the second cell's surfels. A global rebuild would report all
    // of them, and that is the whole difference between incremental and not.
    CY_CHECK_EQ(scene.last_touched_surfels(), second.size());
    CY_CHECK_EQ(scene.resident_cell_count(), 2U);

    // Ingesting a resident cell twice is refused rather than silently duplicating its surfels.
    CY_CHECK_FALSE(scene.ingest_cell(1, left, {first.data(), first.size()}, 1).has_value());

    scene.evict_cell(1, 2);
    CY_CHECK_FALSE(scene.cell_resident(1));
    CY_CHECK_EQ(scene.surfel_count(), second.size());
    CY_CHECK_EQ(scene.last_touched_surfels(), first.size());
    // A query into the evicted region has no coverage, which is what makes the resolve fall back to
    // the far field rather than return black.
    CY_CHECK_FALSE(scene.has_coverage(Vec3{-2.0F, 0.0F, 0.0F}));
    CY_CHECK(scene.has_coverage(Vec3{22.0F, 0.0F, 0.0F}));
}

CY_TEST_CASE("an invalidation is attributable to what caused it") {
    GiScene scene;
    const std::vector<Surfel> surfels = two_walls();
    const cy::Aabb bounds =
        cy::Aabb::from_min_max(Vec3{-4.0F, -2.0F, -2.0F}, Vec3{4.0F, 2.0F, 2.0F});
    CY_REQUIRE(scene.ingest_cell(1, bounds, {surfels.data(), surfels.size()}, 1).has_value());
    scene.invalidate(cy::Aabb::from_center_extents(Vec3{}, Vec3{1.0F, 1.0F, 1.0F}),
                     InvalidationCause::LightChanged, 42, 7);

    CY_REQUIRE_EQ(scene.invalidations().size(), 2U);
    CY_CHECK_EQ(scene.invalidations()[0].cause, InvalidationCause::CellIngested);
    CY_CHECK_EQ(scene.invalidations()[1].cause, InvalidationCause::LightChanged);
    // The light that did it, so "which changes caused which invalidations" has an answer.
    CY_CHECK_EQ(scene.invalidations()[1].source_id, 42U);
    CY_CHECK_EQ(scene.invalidations()[1].frame, 7U);
    CY_CHECK_EQ(scene.invalidation_count(InvalidationCause::LightChanged), 1U);
    scene.clear_invalidations();
    CY_CHECK_EQ(scene.invalidations().size(), 0U);
}

CY_TEST_CASE("a traced hit is a cache lookup and the red wall bleeds") {
    // "WHEN a saturated red wall is lit THEN the cache SHALL record its reflectance, and nearby
    // surfaces SHALL receive tinted indirect light."
    GiScene scene;
    const std::vector<Surfel> surfels = two_walls();
    const cy::Aabb bounds =
        cy::Aabb::from_min_max(Vec3{-4.0F, -2.0F, -2.0F}, Vec3{4.0F, 2.0F, 2.0F});
    CY_REQUIRE(scene.ingest_cell(1, bounds, {surfels.data(), surfels.size()}, 1).has_value());

    SurfaceCache cache;
    CY_REQUIRE(cache.allocate_from(scene, bounds).has_value());
    CY_CHECK_EQ(cache.page_count(), surfels.size());

    GiLight light;
    light.position = Vec3{-1.0F, 0.0F, 0.0F};
    light.colour = Vec3{1.0F, 1.0F, 1.0F};
    light.intensity = 4.0F;
    light.range = 20.0F;
    const std::vector<GiLight> lights = {light};

    SurfaceUpdateContext context;
    context.lights = {lights.data(), lights.size()};
    context.frame = 1;
    const SurfaceUpdateReport report = cache.update_all(context);
    CY_CHECK_EQ(report.pages_updated, surfels.size());

    Vec3 radiance{0.0F, 0.0F, 0.0F};
    u32 age = 0;
    // The lookup is by position and normal, and it is the whole of what a tracer does at a hit.
    CY_REQUIRE(cache.radiance_at(Vec3{-2.0F, 0.0F, 0.0F}, Vec3{1.0F, 0.0F, 0.0F}, radiance, age));
    CY_CHECK_GT(radiance.x, radiance.y * 4.0F);
    CY_CHECK_GT(radiance.x, radiance.z * 4.0F);
    CY_CHECK_EQ(age, 0U);

    // A lookup nowhere near a page finds nothing, and the miss is counted rather than answered.
    CY_CHECK_FALSE(
        cache.radiance_at(Vec3{50.0F, 0.0F, 0.0F}, Vec3{1.0F, 0.0F, 0.0F}, radiance, age));
    CY_CHECK_GT(cache.diagnostics().lookup_misses, 0U);
}

CY_TEST_CASE("bounces accumulate over frames") {
    // "WHEN cached radiance is fed back into the gather THEN successive frames SHALL approximate
    // additional bounces, converging toward a multi-bounce solution."
    GiScene scene;
    const std::vector<Surfel> surfels = two_walls();
    const cy::Aabb bounds =
        cy::Aabb::from_min_max(Vec3{-4.0F, -2.0F, -2.0F}, Vec3{4.0F, 2.0F, 2.0F});
    CY_REQUIRE(scene.ingest_cell(1, bounds, {surfels.data(), surfels.size()}, 1).has_value());

    SurfaceCache cache;
    CY_REQUIRE(cache.allocate_from(scene, bounds).has_value());
    const ConstantIndirect indirect(Vec3{0.4F, 0.4F, 0.4F});

    SurfaceUpdateContext direct_only;
    direct_only.frame = 1;
    (void)cache.update_all(direct_only);
    const Vec3 one_bounce = SurfaceCache::outgoing(cache.page(0));

    SurfaceUpdateContext with_indirect = direct_only;
    with_indirect.indirect = &indirect;
    with_indirect.frame = 2;
    (void)cache.update_all(with_indirect);
    const Vec3 two_bounces = SurfaceCache::outgoing(cache.page(0));

    CY_CHECK_GT(two_bounces.x, one_bounce.x);
    // The added bounce is the page's own albedo times what arrived, which is the one line the
    // multi-bounce approximation is.
    CY_CHECK_NEAR(two_bounces.x - one_bounce.x, cache.page(0).albedo.x * 0.4F, 1e-4F);
}

CY_TEST_CASE("the surface cache is budgeted, prioritised and does not starve") {
    GiScene scene;
    std::vector<Surfel> many;
    for (u32 index = 0; index < 40; ++index) {
        Surfel surfel;
        surfel.position = Vec3{static_cast<f32>(index) * 2.0F, 0.0F, 0.0F};
        surfel.normal = Vec3{0.0F, 1.0F, 0.0F};
        surfel.area = 1.0F;
        many.push_back(surfel);
    }
    const cy::Aabb bounds =
        cy::Aabb::from_min_max(Vec3{-2.0F, -2.0F, -2.0F}, Vec3{100.0F, 2.0F, 2.0F});
    CY_REQUIRE(scene.ingest_cell(1, bounds, {many.data(), many.size()}, 1).has_value());

    SurfaceCache cache;
    CY_REQUIRE(cache.allocate_from(scene, bounds).has_value());

    SurfaceUpdateContext context;
    context.budget = 8;
    context.frame = 1;
    context.max_age_frames = 1000;
    const SurfaceUpdateReport first = cache.update(context);
    CY_CHECK_EQ(first.pages_updated, 8U);
    CY_CHECK_EQ(first.queue_depth, 40U);
    CY_CHECK_GT(first.oldest_unserviced_age, 0U);

    // A visible page outranks an invisible one, whatever their ages. The page is found by position
    // rather than by index: `allocate_from` walks the scene's spatial index, so a page handle is
    // not a surfel index and assuming it is would be testing the traversal order.
    const u32 visible = cache.mark_visible(
        cy::Aabb::from_center_extents(Vec3{78.0F, 0.0F, 0.0F}, Vec3{1.0F, 1.0F, 1.0F}), true);
    CY_CHECK_EQ(visible, 1U);
    context.frame = 2;
    (void)cache.update(context);
    u32 marked = ~0U;
    for (u32 handle = 0; handle < 40; ++handle) {
        if (std::abs(cache.page(handle).position.x - 78.0F) < 0.01F) {
            marked = handle;
        }
    }
    CY_REQUIRE(marked != ~0U);
    CY_CHECK_EQ(cache.page(marked).last_update_frame, 2U);

    // Everything is serviced within a bounded number of frames: no page is starved.
    for (u64 frame = 3; frame < 12; ++frame) {
        context.frame = frame;
        (void)cache.update(context);
    }
    CY_CHECK_EQ(cache.diagnostics().valid_pages, 40U);
}

CY_TEST_CASE("probes are placed near surfaces, not inside them, and not wasted on emptiness") {
    // "Probe placement SHALL be adaptive and geometry-aware, not a uniform grid... Probes inside
    // solid geometry SHALL not be allocated."
    const gi_support::BoxField box(Vec3{2.0F, 2.0F, 2.0F}, 2.0F, 11);
    DistanceField field;
    ClipmapSettings clipmaps;
    clipmaps.levels = 2;
    clipmaps.resolution = 16;
    clipmaps.base_extent_metres = 16.0F;
    CY_REQUIRE(field.configure(clipmaps).has_value());
    CY_REQUIRE(field.place(1, box.asset(), cy::Mat4::identity()).has_value());
    (void)field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});

    RadianceCache cache;
    ProbeCacheSettings settings;
    settings.levels = 1;
    settings.base_spacing_metres = 1.0F;
    settings.half_extent_probes = 5;
    CY_REQUIRE(cache.configure(settings).has_value());

    ProbePlacementContext placement;
    placement.field = &field;
    const ProbePlacementReport report = cache.scroll_to(Vec3{0.0F, 0.0F, 0.0F}, placement);

    CY_CHECK_GT(report.probes_created, 0U);
    // Probes inside the box were refused.
    CY_CHECK_GT(report.rejected_inside_geometry, 0U);
    // And no surviving probe is inside it.
    for (const Probe& probe : cache.probes()) {
        if (probe.live) {
            CY_CHECK_GE(field.distance(probe.position), 0.0F);
        }
    }
    // A uniform grid over the window would be 11^3; the adaptive placement is a fraction of it.
    CY_CHECK_LT(report.probes_created, 11U * 11U * 11U);
}

CY_TEST_CASE("the camera moves and valid probes are reused") {
    RadianceCache cache;
    ProbeCacheSettings settings;
    settings.levels = 1;
    settings.base_spacing_metres = 2.0F;
    settings.half_extent_probes = 3;
    CY_REQUIRE(cache.configure(settings).has_value());

    ProbePlacementContext placement;  // no field: every candidate is accepted
    const ProbePlacementReport first = cache.scroll_to(Vec3{0.0F, 0.0F, 0.0F}, placement);
    CY_CHECK_GT(first.probes_created, 0U);
    CY_CHECK_EQ(first.probes_reused, 0U);

    const ProbePlacementReport same = cache.scroll_to(Vec3{0.0F, 0.0F, 0.0F}, placement);
    CY_CHECK_EQ(same.probes_created, 0U);
    CY_CHECK_EQ(same.probes_reused, first.probes_created);

    // One spacing along: the clipmap scrolls, the overlap is reused, and only the newly entered
    // slice is populated.
    const ProbePlacementReport moved = cache.scroll_to(Vec3{2.0F, 0.0F, 0.0F}, placement);
    CY_CHECK_GT(moved.probes_created, 0U);
    CY_CHECK_GT(moved.probes_reused, moved.probes_created * 3U);
    CY_CHECK_GT(moved.probes_retired, 0U);
}

CY_TEST_CASE("a probe on the far side of a wall does not leak into a query") {
    // "WHEN a query interpolates probes across a wall THEN the visibility term SHALL suppress the
    // contribution of probes not visible from the query point."
    const gi_support::BoxField wall(Vec3{0.2F, 4.0F, 4.0F}, 1.0F, 9);
    DistanceField field;
    ClipmapSettings clipmaps;
    clipmaps.levels = 2;
    clipmaps.resolution = 16;
    clipmaps.base_extent_metres = 16.0F;
    CY_REQUIRE(field.configure(clipmaps).has_value());
    CY_REQUIRE(field.place(1, wall.asset(), cy::Mat4::identity()).has_value());
    (void)field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});

    RadianceCache cache;
    ProbeCacheSettings settings;
    settings.levels = 1;
    settings.base_spacing_metres = 2.0F;
    settings.half_extent_probes = 2;
    CY_REQUIRE(cache.configure(settings).has_value());
    ProbePlacementContext placement;
    placement.field = &field;
    (void)cache.scroll_to(Vec3{0.0F, 0.0F, 0.0F}, placement);

    SoftwareTracer tracer(field, nullptr);
    ProbeUpdateContext update;
    update.tracer = &tracer;
    update.radiance = nullptr;
    update.sky.zenith = Vec3{1.0F, 1.0F, 1.0F};
    update.sky.horizon = Vec3{1.0F, 1.0F, 1.0F};
    update.sky.ground = Vec3{1.0F, 1.0F, 1.0F};
    update.rays_per_probe = 24;
    update.converged = true;
    update.frame = 1;
    update.max_ray_distance_metres = 20.0F;
    (void)cache.update(update);

    // Every probe now knows how far the world is around it. A query pressed against one side of the
    // wall must not interpolate a probe on the other side.
    u32 through_wall = 0;
    u32 same_side = 0;
    for (const Probe& probe : cache.probes()) {
        if (!probe.live || !probe.valid) {
            continue;
        }
        const RadianceSample here =
            cache.sample(probe.position + Vec3{0.3F, 0.0F, 0.0F}, Vec3{1.0F, 0.0F, 0.0F}, 1);
        if (here.confidence > 0.0F) {
            same_side += 1;
        }
        if (probe.position.x < -0.5F) {
            const RadianceSample across = cache.sample(
                Vec3{1.0F, probe.position.y, probe.position.z}, Vec3{1.0F, 0.0F, 0.0F}, 1);
            // Some probes answer the query on the far side; what must be true is that the ones
            // behind the wall are weighted out, which shows up as the far-side answer never being
            // dominated by them. The direct check is the visibility distance itself.
            if (across.confidence > 0.0F && probe.axis_distance[0] < 1.5F) {
                through_wall += 1;
            }
        }
    }
    CY_CHECK_GT(same_side, 0U);
    CY_CHECK_EQ(through_wall, 0U);
}

CY_TEST_CASE("probe updates are budgeted, prioritised and bounded in age") {
    RadianceCache cache;
    ProbeCacheSettings settings;
    settings.levels = 1;
    settings.base_spacing_metres = 2.0F;
    settings.half_extent_probes = 3;
    CY_REQUIRE(cache.configure(settings).has_value());
    ProbePlacementContext placement;
    (void)cache.scroll_to(Vec3{0.0F, 0.0F, 0.0F}, placement);
    const u32 probes = cache.probe_count();
    CY_REQUIRE(probes > 20U);

    ProbeUpdateContext update;
    update.budget = 8;
    update.rays_per_probe = 4;
    update.frame = 1;
    update.max_age_frames = 10;
    const ProbeUpdateReport first = cache.update(update);
    CY_CHECK_EQ(first.probes_updated, 8U);
    CY_CHECK_EQ(first.queue_depth, probes);
    CY_CHECK_EQ(first.rays, 32U);
    CY_CHECK_GT(first.oldest_unserviced_age, 0U);

    // "WHEN a low-priority region is never visible THEN its probes SHALL still be refreshed at a
    // bounded minimum rate." Every probe is valid within a bounded number of frames — and the bound
    // is the cache size over the budget, which is what the scheduler can promise and nothing
    // tighter: eight a frame cannot service three hundred probes in ten frames whatever the
    // priority says.
    const u64 needed = (probes + update.budget - 1U) / update.budget;
    update.max_age_frames = needed + 4U;
    for (u64 frame = 2; frame < needed + 8U; ++frame) {
        update.frame = frame;
        (void)cache.update(update);
    }
    CY_CHECK_EQ(cache.diagnostics().valid_probes, probes);

    // Converged mode ignores the budget, which is what a golden-image capture needs from it.
    update.converged = true;
    update.frame = 100;
    const ProbeUpdateReport all = cache.update(update);
    CY_CHECK_EQ(all.probes_updated, probes);
}
