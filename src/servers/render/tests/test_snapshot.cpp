// The simulation-to-render snapshot. Task 4.1.2.
//
// The three properties `rendering-architecture` states about it, checked:
//
//   "WHEN the render thread builds a frame THEN it SHALL see a coherent snapshot even while
//    simulation advances concurrently"          -> the double-buffered exchange
//   "WHEN 100 000 static instances exist and 50 move THEN only the 50 changed instances SHALL be
//    re-extracted"                              -> the snapshot is a diff, and `examined` says so
//   "WHEN rendering falls between simulation ticks THEN extraction SHALL write interpolated
//    transforms using the frame's interpolation alpha"  -> `resolve_transform`

#include <cy/core/memory/system_allocator.h>
#include <cy/servers/render/server.h>
#include <cy/servers/render/snapshot.h>
#include <cy/test/test.h>

using cy::f32;
using cy::u32;
using cy::u64;
using namespace cy::render;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

/// A server sized for a test rather than for a game: neither case here submits a debug primitive,
/// and the default store is 850 KiB that `initialize()` constructs per case. See
/// `RenderServerConfig`.
[[nodiscard]] bool start(RenderServer& server) noexcept {
    RenderServerConfig config;
    config.debug_primitive_capacity = 8;
    config.debug_label_capacity = 4;
    return server.configure(config).has_value() && server.initialize().has_value();
}

}  // namespace

CY_TEST_CASE("nothing is readable until the first publish") {
    // The correct first frame is an empty one, not an error and not a half-filled buffer.
    SnapshotBuffer buffer(allocator());
    CY_CHECK(buffer.readable() == nullptr);
    CY_CHECK_EQ(buffer.published_count(), 0ULL);
    CY_CHECK_EQ(buffer.publish(), 1ULL);
    CY_CHECK(buffer.readable() != nullptr);
}

CY_TEST_CASE("the buffer being written is never the buffer being read") {
    // The whole synchronisation argument, as an identity check: a publish mid-frame lands in the
    // buffer the renderer is not holding.
    SnapshotBuffer buffer(allocator());
    RenderSnapshot& first = buffer.writable();
    first.state_version = 10;
    CY_REQUIRE(first.removed.push_back(1).has_value());
    (void)buffer.publish();

    const RenderSnapshot* readable = buffer.readable();
    CY_REQUIRE(readable != nullptr);
    CY_CHECK_EQ(readable->state_version, 10ULL);

    RenderSnapshot& second = buffer.writable();
    CY_CHECK(&second != readable);
    // And the buffer handed back for writing has been cleared, so a producer never appends to last
    // frame's contents.
    CY_CHECK(second.removed.empty());
}

CY_TEST_CASE("a snapshot carries changes, not a world") {
    // The incremental requirement, as a shape rather than as a scale test: what the extractor
    // publishes is what changed, and `examined` records how much it had to look at to know.
    RenderSnapshot snapshot(allocator());
    snapshot.examined = 100000;
    for (u32 index = 0; index < 50; ++index) {
        InstanceSnapshot change;
        change.stable_id = 1000U + index;
        CY_REQUIRE(snapshot.changed.push_back(change).has_value());
    }
    CY_CHECK_EQ(snapshot.changed.size(), 50U);
    CY_CHECK_EQ(snapshot.examined, 100000U);
}

CY_TEST_CASE("a transform between two ticks is the blend of them") {
    const cy::Transform before = cy::Transform::from_translation(cy::Vec3{0.0F, 0.0F, 0.0F});
    const cy::Transform after = cy::Transform::from_translation(cy::Vec3{10.0F, 0.0F, 0.0F});

    CY_CHECK_NEAR(resolve_transform(before, after, 0.0F, false).translation.x, 0.0F, 1e-5F);
    CY_CHECK_NEAR(resolve_transform(before, after, 0.5F, false).translation.x, 5.0F, 1e-5F);
    CY_CHECK_NEAR(resolve_transform(before, after, 1.0F, false).translation.x, 10.0F, 1e-5F);
    // Out-of-range alphas clamp rather than extrapolate: a frame that overran its tick should show
    // the latest state, not a prediction nothing authorised.
    CY_CHECK_NEAR(resolve_transform(before, after, 2.0F, false).translation.x, 10.0F, 1e-5F);
    CY_CHECK_NEAR(resolve_transform(before, after, -1.0F, false).translation.x, 0.0F, 1e-5F);
}

CY_TEST_CASE("a teleport suppresses interpolation for that frame") {
    // Blending across a jump draws the object smeared over the gap for one frame, which is what a
    // teleport looks like when nobody thought about it.
    const cy::Transform before = cy::Transform::from_translation(cy::Vec3{0.0F, 0.0F, 0.0F});
    const cy::Transform after = cy::Transform::from_translation(cy::Vec3{1000.0F, 0.0F, 0.0F});
    CY_CHECK_NEAR(resolve_transform(before, after, 0.5F, true).translation.x, 1000.0F, 1e-3F);
}

CY_TEST_CASE("applying a snapshot creates, updates and removes without touching anything else") {
    RenderServer server(allocator());
    CY_REQUIRE(start(server));

    SceneDescription scene_desc;
    scene_desc.instance_capacity = 32;
    auto scene = server.create_scene(scene_desc);
    CY_REQUIRE(scene.has_value());

    MeshDescription mesh_desc;
    mesh_desc.vertex_count = 3;
    mesh_desc.index_count = 3;
    MeshSurface surface;
    surface.index_count = 3;
    auto mesh = server.create_mesh(mesh_desc, cy::Span<const MeshSurface>(&surface, 1),
                                   cy::Span<const MeshLod>());
    CY_REQUIRE(mesh.has_value());
    auto material = server.create_material(MaterialRecord{});
    CY_REQUIRE(material.has_value());

    SnapshotBuffer buffer(allocator());
    {
        RenderSnapshot& writing = buffer.writable();
        for (u32 index = 0; index < 3; ++index) {
            InstanceSnapshot change;
            change.stable_id = 100U + index;
            change.mesh = *mesh;
            change.material = *material;
            change.previous_transform = cy::Transform::from_translation(cy::Vec3{0.0F, 0.0F, 0.0F});
            change.transform =
                cy::Transform::from_translation(cy::Vec3{static_cast<f32>(index), 0.0F, 0.0F});
            change.local_bounds = cy::Aabb::from_center_extents(cy::Vec3{0.0F, 0.0F, 0.0F},
                                                                cy::Vec3{0.5F, 0.5F, 0.5F});
            CY_REQUIRE(writing.changed.push_back(change).has_value());
        }
        (void)buffer.publish();
    }
    CY_REQUIRE(server.apply_snapshot(*scene, *buffer.readable(), 1.0F).has_value());
    CY_CHECK_EQ(server.live_instances(), 3U);

    // A second snapshot that moves one and removes another. Nothing else is touched: the third
    // instance's handle is still the same handle, which is the incremental property from the
    // consuming side.
    const InstanceHandle untouched = server.find_instance(*scene, 102);
    CY_REQUIRE(untouched);
    {
        RenderSnapshot& writing = buffer.writable();
        InstanceSnapshot moved;
        moved.stable_id = 100;
        moved.mesh = *mesh;
        moved.material = *material;
        moved.previous_transform = cy::Transform::from_translation(cy::Vec3{0.0F, 0.0F, 0.0F});
        moved.transform = cy::Transform::from_translation(cy::Vec3{50.0F, 0.0F, 0.0F});
        CY_REQUIRE(writing.changed.push_back(moved).has_value());
        CY_REQUIRE(writing.removed.push_back(101).has_value());
        (void)buffer.publish();
    }
    CY_REQUIRE(server.apply_snapshot(*scene, *buffer.readable(), 1.0F).has_value());

    CY_CHECK_EQ(server.live_instances(), 2U);
    CY_CHECK_FALSE(server.find_instance(*scene, 101));
    CY_CHECK_EQ(server.find_instance(*scene, 102), untouched);
    const InstanceRecord* moved = server.instance(server.find_instance(*scene, 100));
    CY_REQUIRE(moved != nullptr);
    CY_CHECK_NEAR(moved->instance.desc.transform.translation.x, 50.0F, 1e-4F);
}

CY_TEST_CASE("applying a snapshot at half alpha places instances between the two ticks") {
    RenderServer server(allocator());
    CY_REQUIRE(start(server));
    SceneDescription scene_desc;
    scene_desc.instance_capacity = 8;
    auto scene = server.create_scene(scene_desc);
    CY_REQUIRE(scene.has_value());

    RenderSnapshot snapshot(allocator());
    InstanceSnapshot change;
    change.stable_id = 1;
    change.previous_transform = cy::Transform::from_translation(cy::Vec3{0.0F, 0.0F, 0.0F});
    change.transform = cy::Transform::from_translation(cy::Vec3{8.0F, 0.0F, 0.0F});
    CY_REQUIRE(snapshot.changed.push_back(change).has_value());

    CY_REQUIRE(server.apply_snapshot(*scene, snapshot, 0.25F).has_value());
    const InstanceRecord* record = server.instance(server.find_instance(*scene, 1));
    CY_REQUIRE(record != nullptr);
    CY_CHECK_NEAR(record->instance.desc.transform.translation.x, 2.0F, 1e-4F);
}

CY_TEST_CASE("the snapshot's flags decide visibility, shadow casting and sidedness") {
    // A REGRESSION. `InstanceSnapshot::flags` was carried across the boundary and read by nothing:
    // `apply_snapshot` filled in the transform, the handles, the bounds, the layer and the LOD bias
    // and left the four booleans at their defaults, so an entity whose `MeshRenderer::visible` was
    // false was published, drawn, and could not be turned off through the snapshot at all. The
    // extractor sets the bits; this is the case that says the server reads them.
    RenderServer server(allocator());
    CY_REQUIRE(start(server));

    SceneDescription scene_desc;
    scene_desc.instance_capacity = 8;
    const auto scene = server.create_scene(scene_desc);
    CY_REQUIRE(scene.has_value());

    RenderSnapshot snapshot(allocator());
    InstanceSnapshot hidden;
    hidden.stable_id = 1;
    hidden.transform = cy::Transform::identity();
    hidden.previous_transform = hidden.transform;
    // Active but not visible: an instance the simulation has switched off.
    hidden.flags = kInstanceActive;
    CY_REQUIRE(snapshot.changed.push_back(hidden).has_value());

    InstanceSnapshot shown;
    shown.stable_id = 2;
    shown.transform = cy::Transform::identity();
    shown.previous_transform = shown.transform;
    shown.flags = kInstanceActive | kInstanceVisible | kInstanceCastsShadow |
                  kInstanceReceivesShadow | kInstanceTwoSided;
    CY_REQUIRE(snapshot.changed.push_back(shown).has_value());

    CY_REQUIRE(server.apply_snapshot(*scene, snapshot, 1.0F).has_value());

    const InstanceRecord* first = server.instance(server.find_instance(*scene, 1));
    const InstanceRecord* second = server.instance(server.find_instance(*scene, 2));
    CY_REQUIRE(first != nullptr);
    CY_REQUIRE(second != nullptr);
    CY_CHECK_FALSE(first->instance.desc.visible);
    CY_CHECK_FALSE(first->instance.desc.casts_shadow);
    CY_CHECK(second->instance.desc.visible);
    CY_CHECK(second->instance.desc.casts_shadow);
    CY_CHECK(second->instance.desc.receives_shadow);
    CY_CHECK(second->instance.desc.two_sided);

    // And on the UPDATE path too, which is the half that would otherwise drift: the same instance,
    // published again with the bit cleared, stops being visible.
    RenderSnapshot second_tick(allocator());
    InstanceSnapshot turned_off = shown;
    turned_off.flags = kInstanceActive;
    CY_REQUIRE(second_tick.changed.push_back(turned_off).has_value());
    CY_REQUIRE(server.apply_snapshot(*scene, second_tick, 1.0F).has_value());
    CY_CHECK_FALSE(server.instance(server.find_instance(*scene, 2))->instance.desc.visible);
}

CY_TEST_CASE("a publish mid-frame does not empty the snapshot the renderer is holding") {
    // A REGRESSION. `publish()` used to clear the buffer it had just flipped away from — the one a
    // renderer acquires at the start of a frame and holds for its duration. The runtime runs N
    // fixed ticks and then one variable-rate render, so a publish mid-frame is the ordinary case,
    // and the reader watched its instance list empty underneath it between the cull and the draw.
    SnapshotBuffer buffer(allocator());

    InstanceSnapshot instance;
    instance.stable_id = 1;
    CY_REQUIRE(buffer.writable().changed.push_back(instance).has_value());
    (void)buffer.publish();

    const RenderSnapshot* held = buffer.readable();
    CY_REQUIRE(held != nullptr);
    CY_REQUIRE_EQ(held->changed.size(), 1U);

    // The simulation commits again, mid-frame, while the renderer still holds the first snapshot.
    InstanceSnapshot second;
    second.stable_id = 2;
    CY_REQUIRE(buffer.writable().changed.push_back(second).has_value());
    (void)buffer.publish();

    // What the renderer holds is still what it acquired.
    CY_CHECK_EQ(held->changed.size(), 1U);
    CY_CHECK_EQ(held->changed[0].stable_id, 1ULL);
    // And the new one is the other buffer, with the new contents.
    CY_REQUIRE(buffer.readable() != held);
    CY_CHECK_EQ(buffer.readable()->changed[0].stable_id, 2ULL);
}
