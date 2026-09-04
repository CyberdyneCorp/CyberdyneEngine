// The spatial index and the per-view cull. Task 4.4.3.
//
// Every case builds a real index and a real frustum; nothing is mocked, because the thing under
// test is the join between the dense arrays, the tree and the frustum, and a mock of any of them
// would test the mock.

#include <cy/test/test.h>

#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/culling/cull.h>

namespace {

using cy::rendering::CullOptions;
using cy::rendering::CullResults;
using cy::rendering::CullView;
using cy::rendering::CullWorkspace;
using cy::rendering::SpatialEntry;
using cy::rendering::SpatialIndex;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

/// A camera at the origin looking down −Z, with a 60° vertical field of view.
CullView make_view() noexcept {
    CullView view;
    const cy::Mat4 projection = cy::perspective_reversed_z(1.0471975512F, 1.0F, 0.1F, 1000.0F);
    const cy::Mat4 camera = cy::look_at(cy::Vec3{0.0F, 0.0F, 0.0F}, cy::Vec3{0.0F, 0.0F, -1.0F});
    view.frustum = cy::Frustum::from_view_projection(projection * camera);
    view.camera_position = cy::Vec3{0.0F, 0.0F, 0.0F};
    view.camera_forward = cy::Vec3{0.0F, 0.0F, -1.0F};
    view.lod.hysteresis = 0.0F;
    return view;
}

SpatialEntry make_entry(cy::Vec3 center, cy::u64 id) noexcept {
    SpatialEntry entry;
    entry.bounds = cy::Aabb::from_center_extents(center, cy::Vec3{0.5F, 0.5F, 0.5F});
    entry.stable_id = id;
    entry.gpu_slot = static_cast<cy::u32>(id);
    return entry;
}

}  // namespace

CY_TEST_CASE("a slot is stable, and removal returns it to the free list") {
    SpatialIndex index(allocator());
    const cy::Expected<cy::u32, cy::Error> first = index.insert(make_entry({0, 0, -5}, 1));
    CY_REQUIRE(first.has_value());
    const cy::Expected<cy::u32, cy::Error> second = index.insert(make_entry({1, 0, -5}, 2));
    CY_REQUIRE(second.has_value());
    CY_CHECK_NE(*first, *second);
    CY_CHECK_EQ(index.entry(*first).stable_id, 1U);

    CY_REQUIRE(index.remove(*first).has_value());
    CY_CHECK_EQ(index.statistics().free_slots, 1U);
    // The freed slot is handed out again, which is the documented contract — a caller that keeps
    // slots across removals needs its own generation.
    const cy::Expected<cy::u32, cy::Error> third = index.insert(make_entry({2, 0, -5}, 3));
    CY_REQUIRE(third.has_value());
    CY_CHECK_EQ(*third, *first);
    CY_CHECK_EQ(index.entry(*third).stable_id, 3U);
}

CY_TEST_CASE("a small movement does not restructure the tree") {
    // The scenario `core-math` documents from the tree's side and `rendering-culling-and-lod` from
    // the scene's: an object moving inside its fattened bounds costs nothing.
    SpatialIndex index(allocator());
    const cy::Expected<cy::u32, cy::Error> slot = index.insert(make_entry({0, 0, -5}, 1));
    CY_REQUIRE(slot.has_value());
    const cy::u64 before = index.statistics().tree_restructures;

    CY_REQUIRE(index
                   .update(*slot, cy::Aabb::from_center_extents(cy::Vec3{0.01F, 0.0F, -5.0F},
                                                                cy::Vec3{0.5F, 0.5F, 0.5F}))
                   .has_value());
    CY_CHECK_EQ(index.statistics().tree_restructures, before);

    // A movement well outside the margin does restructure, which is what makes the first assertion
    // a statement about the margin rather than about the tree being inert.
    CY_REQUIRE(index
                   .update(*slot, cy::Aabb::from_center_extents(cy::Vec3{50.0F, 0.0F, -5.0F},
                                                                cy::Vec3{0.5F, 0.5F, 0.5F}))
                   .has_value());
    CY_CHECK_GT(index.statistics().tree_restructures, before);
}

CY_TEST_CASE("the layer test rejects before any geometry is touched") {
    SpatialIndex index(allocator());
    SpatialEntry inside = make_entry({0, 0, -5}, 1);
    inside.layer_mask = 1U << 3U;
    CY_REQUIRE(index.insert(inside).has_value());

    CullView view = make_view();
    view.layer_mask = 1U << 4U;  // a mask that shares no bit

    CullWorkspace workspace(allocator());
    CullResults results(allocator());
    CY_REQUIRE(cull_view(index, view, CullOptions{}, workspace, results).has_value());

    CY_CHECK_EQ(results.stats.tested, 1U);
    CY_CHECK_EQ(results.stats.rejected_by_layer, 1U);
    // The instance is inside the frustum, so a frustum rejection here would mean the tests ran in
    // the wrong order.
    CY_CHECK_EQ(results.stats.rejected_by_frustum, 0U);
    CY_CHECK_EQ(results.stats.visible, 0U);
}

CY_TEST_CASE("the frustum keeps what is in front and rejects what is behind") {
    SpatialIndex index(allocator());
    CY_REQUIRE(index.insert(make_entry({0, 0, -5}, 1)).has_value());     // in front
    CY_REQUIRE(index.insert(make_entry({0, 0, 5}, 2)).has_value());      // behind the camera
    CY_REQUIRE(index.insert(make_entry({0, 0, -2000}, 3)).has_value());  // past the far plane

    CullWorkspace workspace(allocator());
    CullResults results(allocator());
    CY_REQUIRE(cull_view(index, make_view(), CullOptions{}, workspace, results).has_value());

    CY_CHECK_EQ(results.stats.tested, 3U);
    CY_CHECK_EQ(results.stats.visible, 1U);
    CY_REQUIRE_EQ(results.opaque.size(), 1U);
    CY_CHECK_EQ(results.opaque[0].stable_id, 1U);
    CY_CHECK_NEAR(results.opaque[0].view_depth, 5.0F, 1e-4F);
}

CY_TEST_CASE("a per-instance distance limit is applied after the frustum") {
    SpatialIndex index(allocator());
    SpatialEntry near_entry = make_entry({0, 0, -5}, 1);
    SpatialEntry far_entry = make_entry({0, 0, -50}, 2);
    far_entry.max_draw_distance = 10.0F;
    CY_REQUIRE(index.insert(near_entry).has_value());
    CY_REQUIRE(index.insert(far_entry).has_value());

    CullWorkspace workspace(allocator());
    CullResults results(allocator());
    CY_REQUIRE(cull_view(index, make_view(), CullOptions{}, workspace, results).has_value());

    CY_CHECK_EQ(results.stats.rejected_by_range, 1U);
    CY_CHECK_EQ(results.stats.rejected_by_frustum, 0U);
    CY_REQUIRE_EQ(results.opaque.size(), 1U);
    CY_CHECK_EQ(results.opaque[0].stable_id, 1U);
}

CY_TEST_CASE("survivors are routed into their typed lists") {
    SpatialIndex index(allocator());
    SpatialEntry opaque = make_entry({0, 1, -5}, 1);
    SpatialEntry transparent = make_entry({0, -1, -5}, 2);
    transparent.flags |= cy::rendering::kSpatialTransparent;
    SpatialEntry mover = make_entry({1, 0, -5}, 3);
    mover.flags |= cy::rendering::kSpatialMoved;
    SpatialEntry light = make_entry({-1, 0, -5}, 4);
    light.domain = cy::rendering::SpatialDomain::Volume;
    light.volume_kind = cy::rendering::VolumeKind::Light;
    SpatialEntry decal = make_entry({-2, 0, -5}, 5);
    decal.domain = cy::rendering::SpatialDomain::Volume;
    decal.volume_kind = cy::rendering::VolumeKind::Decal;

    for (const SpatialEntry& entry : {opaque, transparent, mover, light, decal}) {
        CY_REQUIRE(index.insert(entry).has_value());
    }

    CullWorkspace workspace(allocator());
    CullResults results(allocator());
    CY_REQUIRE(cull_view(index, make_view(), CullOptions{}, workspace, results).has_value());

    CY_CHECK_EQ(results.opaque.size(), 2U);  // the plain one and the mover
    CY_CHECK_EQ(results.transparent.size(), 1U);
    CY_CHECK_EQ(results.motion.size(), 1U);
    CY_CHECK_EQ(results.lights.size(), 1U);
    CY_CHECK_EQ(results.decals.size(), 1U);
    // A moving opaque instance is in BOTH lists: motion is a subset, not a partition.
    CY_CHECK_EQ(results.motion[0].stable_id, 3U);
}

CY_TEST_CASE("an always-visible instance bypasses the tree but not the layer mask") {
    SpatialIndex index(allocator());
    SpatialEntry sky = make_entry({0, 0, 1000}, 1);  // behind the camera, and drawn anyway
    sky.flags |= cy::rendering::kSpatialAlwaysVisible;
    CY_REQUIRE(index.insert(sky).has_value());
    CY_CHECK_EQ(index.statistics().always_visible, 1U);
    CY_CHECK_EQ(index.statistics().renderables, 0U);
}

CY_TEST_CASE("a shadow caster that cannot reach the camera frustum is rejected") {
    // "WHEN a caster is inside the light's volume but cannot project into the camera frustum THEN
    // it SHALL be excluded from the shadow render list."
    SpatialIndex index(allocator());
    CY_REQUIRE(index.insert(make_entry({0, 5, -10}, 1)).has_value());  // above the view, casts in
    CY_REQUIRE(index.insert(make_entry({0, 5, 200}, 2)).has_value());  // behind, casts nowhere

    cy::rendering::ShadowCullView shadow;
    // A shadow frustum wide enough to contain both, so the rejection is the sweep's and not the
    // shadow projection's.
    const cy::Mat4 light_projection = cy::orthographic_reversed_z(-500, 500, -500, 500, 0.0F, 1000);
    // The up vector is not the world's: a light shining straight down is parallel to it, and
    // `look_at` has nothing to cross against. `compute_cascades` picks one for the same reason.
    const cy::Mat4 light_view = cy::look_at(
        cy::Vec3{0.0F, 400.0F, 0.0F}, cy::Vec3{0.0F, 0.0F, 0.0F}, cy::Vec3{0.0F, 0.0F, -1.0F});
    shadow.shadow_frustum = cy::Frustum::from_view_projection(light_projection * light_view);
    shadow.camera_frustum = make_view().frustum;
    shadow.light_direction = cy::Vec3{0.0F, -1.0F, 0.0F};
    shadow.sweep_distance = 20.0F;

    cy::Array<cy::rendering::VisibleInstance> casters(allocator());
    cy::rendering::ShadowCullStatistics stats;
    CY_REQUIRE(cull_shadow_casters(index, shadow, casters, stats).has_value());

    CY_CHECK_EQ(stats.tested, 2U);
    CY_CHECK_EQ(stats.casters, 1U);
    CY_CHECK_EQ(stats.rejected_by_sweep, 1U);
    CY_REQUIRE_EQ(casters.size(), 1U);
    CY_CHECK_EQ(casters[0].stable_id, 1U);

    // With the tighter test off — the shared-shadow-map case — the second caster comes back, which
    // is what makes one shadow map valid for several camera views.
    shadow.tight = false;
    CY_REQUIRE(cull_shadow_casters(index, shadow, casters, stats).has_value());
    CY_CHECK_EQ(stats.casters, 2U);
    CY_CHECK_EQ(stats.rejected_by_sweep, 0U);
}
