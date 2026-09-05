// The extract stage: an ECS world becomes a render snapshot at the commit boundary. Task 4.1.2.
//
// The four things `rendering-architecture` says about the snapshot, checked here:
//
//   "Rendering SHALL consume an immutable render snapshot published at a defined point each frame"
//        -> the extractor is a `CommitObserver` and publishes from `CommitBoundary::commit()`
//   "WHEN 100 000 static instances exist and 50 move THEN only the 50 changed instances SHALL be
//    re-extracted"                                    -> `ExtractStatistics::examined`
//   "WHEN rendering falls between simulation ticks THEN extraction SHALL write interpolated
//    transforms using the frame's interpolation alpha" -> both placements cross the boundary
//   "WHEN an effect, entity, or UI document is destroyed THEN its instances SHALL be removed"
//        -> the removal sweep
//
// AND THE ONE THAT IS NOT IN THE SPECIFICATION BUT IS IN design.md §6: the snapshot's contents do
// not depend on iteration order over anything a hash map decides. The last case runs two identical
// worlds and compares the two snapshots element by element.

#include <cy/core/determinism/commit.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/scene/extract.h>
#include <cy/scene/tree.h>
#include <cy/test/test.h>

using cy::f32;
using cy::u32;
using cy::u64;
using namespace cy::rendering;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

/// The world, the scene tree, and the two sets of component ids — initialised as a BASE-like member
/// so they exist before the extractor that binds to them. The extractor caches its ids, so it has
/// to be constructed after registration; a member declared after this one is, which is the whole
/// reason this struct is separate from `Fixture`.
struct Bindings {
    Bindings(cy::ecs::World& world, cy::scene::SceneTree& tree) noexcept {
        ok = world.initialize().has_value() && tree.initialize().has_value();
        if (!ok) {
            return;
        }
        scene = tree.components();
        auto registered = RenderComponents::register_all(world);
        ok = registered.has_value();
        if (ok) {
            render = *registered;
        }
    }

    cy::scene::SceneComponents scene;
    RenderComponents render;
    bool ok = false;
};

/// A world, a scene tree over it, the renderer's components, a snapshot buffer and an extractor.
///
/// Nothing here is a mock: the world is the ECS's, the transforms are the scene layer's and the
/// snapshot is the render server's. The extractor is the only thing under test, which is what makes
/// the cases below claims about the engine rather than about a fixture.
struct Fixture {
    Fixture() noexcept
        : world(allocator()),
          tree(world),
          bindings(world, tree),
          buffer(allocator()),
          extractor(allocator(), world, bindings.render, bindings.scene, buffer) {}

    [[nodiscard]] bool start() const noexcept { return bindings.ok; }
    [[nodiscard]] const cy::scene::SceneComponents& scene() const noexcept {
        return bindings.scene;
    }
    [[nodiscard]] const RenderComponents& render() const noexcept { return bindings.render; }

    /// An entity with a placement and a mesh. `interpolated` gives it the opt-in previous-transform
    /// component, which is what a node marked interpolatable carries.
    [[nodiscard]] cy::ecs::Entity spawn(cy::Vec3 position, bool interpolated = false) noexcept {
        cy::ecs::ComponentTypeId components[3] = {scene().world_transform, render().mesh_renderer,
                                                  scene().interpolated_transform};
        auto entity = world.create(
            cy::Span<const cy::ecs::ComponentTypeId>(components, interpolated ? 3U : 2U));
        if (!entity) {
            return cy::ecs::Entity{};
        }
        cy::scene::WorldTransform transform;
        transform.value = cy::Transform::from_translation(position);
        if (!world.set(*entity, scene().world_transform, transform).has_value()) {
            return cy::ecs::Entity{};
        }
        MeshRenderer renderer;
        renderer.mesh = cy::render::MeshHandle::from_slot(1, 1);
        renderer.material = cy::render::MaterialHandle::from_slot(2, 1);
        if (!world.set(*entity, render().mesh_renderer, renderer).has_value()) {
            return cy::ecs::Entity{};
        }
        return *entity;
    }

    /// Commit a tick, which is what makes the extractor run. The version the record carries is what
    /// the snapshot stamps itself with.
    [[nodiscard]] bool commit(u64 version) noexcept {
        cy::determinism::CommitRecord record;
        record.state_version = version;
        return extractor.on_commit(record).has_value();
    }

    [[nodiscard]] const cy::render::RenderSnapshot& published() const noexcept {
        return *buffer.readable();
    }

    cy::ecs::World world;
    cy::scene::SceneTree tree;
    Bindings bindings;
    cy::render::SnapshotBuffer buffer;
    SnapshotExtractor extractor;
};

}  // namespace

CY_TEST_CASE("the renderer's components register once and keep their ids") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_CHECK(fixture.render().registered());

    // Idempotent: a second extractor over one world binds to the same numbers rather than
    // registering a second set of components.
    const auto again = RenderComponents::register_all(fixture.world);
    CY_REQUIRE(again.has_value());
    CY_CHECK_EQ(again->mesh_renderer, fixture.render().mesh_renderer);
    CY_CHECK_EQ(again->light_source, fixture.render().light_source);
    CY_CHECK_EQ(again->camera, fixture.render().camera);
}

CY_TEST_CASE("the first extraction publishes the world and stamps the tick it saw") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.spawn(cy::Vec3{1.0F, 0.0F, 0.0F}).valid());
    CY_REQUIRE(fixture.spawn(cy::Vec3{2.0F, 0.0F, 0.0F}).valid());

    CY_REQUIRE(fixture.commit(7));
    const cy::render::RenderSnapshot& snapshot = fixture.published();
    CY_CHECK_EQ(snapshot.state_version, 7ULL);
    CY_CHECK_EQ(snapshot.changed.size(), 2U);
    CY_CHECK_EQ(snapshot.removed.size(), 0U);
    CY_CHECK_EQ(fixture.extractor.statistics().published, 2U);

    // The placement crossed the boundary, and the instance carries the flags the server reads.
    const cy::render::InstanceSnapshot& instance = snapshot.changed[0];
    CY_CHECK((instance.flags & cy::render::kInstanceActive) != 0U);
    CY_CHECK((instance.flags & cy::render::kInstanceVisible) != 0U);
    CY_CHECK(instance.mesh);
    CY_CHECK_NE(instance.stable_id, 0ULL);
}

CY_TEST_CASE("a static crowd is skipped whole and only the chunks that changed are re-read") {
    // The specification's scenario — "WHEN 100 000 static instances exist and 50 move THEN only the
    // 50 changed instances SHALL be re-extracted" — at a size a unit budget affords, and stated at
    // the granularity the ECS actually offers.
    //
    // CHANGE DETECTION IS CHUNK-GRANULAR, and `ecs-core` says so in as many words: "WHEN a
    // component is written THEN the whole chunk SHALL be considered changed". So the honest claim
    // is not "two rows were re-extracted" but "the chunks that did not change were not read at
    // all", and the fixture makes that visible by putting the movers in a different ARCHETYPE — an
    // entity that opted into interpolation — from the hundred that stay still.
    // THIRTY-TWO AND NOT A HUNDRED, and the number is a measurement rather than a preference. At a
    // hundred this case cost 0.87 to 1.03 ms of CPU at the Debug profile's -O0 against the unit
    // suite's 1 ms budget, so it failed `just test-all --profile debug` about one round in five and
    // took `four-profiles` — and therefore every milestone ledger above it — with it. What the case
    // asserts is that a chunk nothing wrote is not read at all, and that property needs more static
    // entities than movers, not a hundred of them.
    static constexpr u32 kStatic = 32;
    Fixture fixture;
    CY_REQUIRE(fixture.start());

    for (u32 index = 0; index < kStatic; ++index) {
        CY_REQUIRE(fixture.spawn(cy::Vec3{static_cast<f32>(index), 0.0F, 0.0F}).valid());
    }
    cy::Array<cy::ecs::Entity> movers(allocator());
    for (u32 index = 0; index < 4; ++index) {
        const cy::ecs::Entity entity = fixture.spawn(cy::Vec3{0.0F, static_cast<f32>(index), 0.0F},
                                                     /*interpolated=*/true);
        CY_REQUIRE(entity.valid());
        CY_REQUIRE(movers.push_back(entity).has_value());
    }

    CY_REQUIRE(fixture.commit(1));
    CY_CHECK_EQ(fixture.published().changed.size(), kStatic + 4U);

    // A tick in which nothing changed: every chunk is skipped whole, and the snapshot is empty.
    CY_REQUIRE(fixture.commit(2));
    CY_CHECK_EQ(fixture.published().changed.size(), 0U);
    CY_CHECK_EQ(fixture.extractor.statistics().examined, 0U);
    CY_CHECK_GT(fixture.extractor.statistics().chunks_skipped, 0U);
    CY_CHECK_EQ(fixture.extractor.statistics().chunks_visited, 0U);

    // Move two of the four. The hundred are in chunks nothing wrote, so they are not read: the
    // extraction is proportional to what changed, which is the requirement.
    for (const u32 index : {0U, 1U}) {
        cy::scene::WorldTransform moved;
        moved.value = cy::Transform::from_translation(cy::Vec3{0.0F, 5.0F, 0.0F});
        CY_REQUIRE(
            fixture.world.set(movers[index], fixture.scene().world_transform, moved).has_value());
    }
    CY_REQUIRE(fixture.commit(3));
    CY_CHECK_EQ(fixture.published().changed.size(), 4U);
    CY_CHECK_EQ(fixture.extractor.statistics().examined, 4U);
    CY_CHECK_EQ(fixture.published().examined, 4U);
    CY_CHECK_EQ(fixture.extractor.statistics().chunks_visited, 1U);
    CY_CHECK_GT(fixture.extractor.statistics().chunks_skipped, 0U);
}

CY_TEST_CASE("changing the renderable and not the transform re-extracts too") {
    // The filter is over two components. A material swap that left the transform alone would
    // otherwise never reach the renderer, and the symptom is an object that keeps its old material
    // until something moves it.
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const cy::ecs::Entity entity = fixture.spawn(cy::Vec3{0.0F, 0.0F, 0.0F});
    CY_REQUIRE(entity.valid());
    CY_REQUIRE(fixture.commit(1));

    auto* renderer = fixture.world.get_mut<MeshRenderer>(entity, fixture.render().mesh_renderer);
    CY_REQUIRE(renderer != nullptr);
    renderer->material = cy::render::MaterialHandle::from_slot(9, 1);

    CY_REQUIRE(fixture.commit(2));
    CY_REQUIRE_EQ(fixture.published().changed.size(), 1U);
    CY_CHECK_EQ(fixture.published().changed[0].material.index(), 9U);
}

CY_TEST_CASE("a destroyed entity is removed, and only ticks with a structural change sweep") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const cy::ecs::Entity first = fixture.spawn(cy::Vec3{0.0F, 0.0F, 0.0F});
    const cy::ecs::Entity second = fixture.spawn(cy::Vec3{1.0F, 0.0F, 0.0F});
    CY_REQUIRE(first.valid());
    CY_REQUIRE(second.valid());
    CY_REQUIRE(fixture.commit(1));
    CY_CHECK_EQ(fixture.extractor.published().size(), 2U);

    // Nothing structural happened, so the sweep does not run at all.
    CY_REQUIRE(fixture.commit(2));
    CY_CHECK_FALSE(fixture.extractor.statistics().swept);
    CY_CHECK_EQ(fixture.published().removed.size(), 0U);

    CY_REQUIRE(fixture.world.destroy(first).has_value());
    CY_REQUIRE(fixture.commit(3));
    CY_CHECK(fixture.extractor.statistics().swept);
    CY_REQUIRE_EQ(fixture.published().removed.size(), 1U);
    CY_CHECK_EQ(fixture.published().removed[0], first.bits());
    CY_CHECK_EQ(fixture.extractor.published().size(), 1U);

    // Losing the component is a removal too: the entity is alive and no longer renderable, and a
    // consumer told about only the destroyed one would keep drawing this.
    CY_REQUIRE(fixture.world.remove(second, fixture.render().mesh_renderer).has_value());
    CY_REQUIRE(fixture.commit(4));
    CY_REQUIRE_EQ(fixture.published().removed.size(), 1U);
    CY_CHECK_EQ(fixture.published().removed[0], second.bits());
    CY_CHECK_EQ(fixture.extractor.published().size(), 0U);
}

CY_TEST_CASE("both placements cross the boundary, so the frame can blend between them") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const cy::ecs::Entity entity = fixture.spawn(cy::Vec3{0.0F, 0.0F, 0.0F}, true);
    CY_REQUIRE(entity.valid());

    // What propagation writes for an interpolatable node: the previous tick's placement, kept as a
    // `Presentation` field so an authoritative system cannot read it back.
    auto* interpolated = fixture.world.get_mut<cy::scene::InterpolatedTransform>(
        entity, fixture.scene().interpolated_transform);
    CY_REQUIRE(interpolated != nullptr);
    interpolated->previous.write(cy::determinism::PresentationContext{},
                                 cy::Transform::from_translation(cy::Vec3{0.0F, 0.0F, -10.0F}));

    CY_REQUIRE(fixture.commit(1));
    CY_REQUIRE_EQ(fixture.published().changed.size(), 1U);
    const cy::render::InstanceSnapshot& instance = fixture.published().changed[0];
    CY_CHECK_NEAR(instance.previous_transform.translation.z, -10.0F, 1e-5F);
    CY_CHECK_NEAR(instance.transform.translation.z, 0.0F, 1e-5F);
    CY_CHECK_FALSE(instance.teleported);

    // Half way between the two ticks, which is where a variable-rate frame usually falls.
    const cy::Transform blended = cy::render::resolve_transform(
        instance.previous_transform, instance.transform, 0.5F, instance.teleported);
    CY_CHECK_NEAR(blended.translation.z, -5.0F, 1e-4F);

    // A teleport suppresses the blend for that frame, and the flag rides the snapshot.
    //
    // RE-ACQUIRED rather than written through the pointer held above, and that is the ECS's
    // contract rather than a quirk of this test: `get_mut` stamps the chunk's version for the
    // component AT THE CALL, so a write through a pointer taken before the previous extraction
    // would not be seen by the change filter. A system that cached a component pointer across a
    // tick would have the same defect, which is why the pointer is taken again here.
    interpolated = fixture.world.get_mut<cy::scene::InterpolatedTransform>(
        entity, fixture.scene().interpolated_transform);
    CY_REQUIRE(interpolated != nullptr);
    interpolated->teleport.write(cy::determinism::PresentationContext{}, true);
    CY_REQUIRE(fixture.commit(2));
    CY_REQUIRE_EQ(fixture.published().changed.size(), 1U);
    CY_CHECK(fixture.published().changed[0].teleported);
}

CY_TEST_CASE("an entity with no interpolation publishes one placement twice, not a smear") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.spawn(cy::Vec3{3.0F, 0.0F, 0.0F}).valid());
    CY_REQUIRE(fixture.commit(1));

    const cy::render::InstanceSnapshot& instance = fixture.published().changed[0];
    CY_CHECK_NEAR(instance.previous_transform.translation.x, 3.0F, 1e-5F);
    CY_CHECK_NEAR(instance.transform.translation.x, 3.0F, 1e-5F);
    // Which blends to the same answer at every alpha rather than to a streak from the origin.
    const cy::Transform blended = cy::render::resolve_transform(instance.previous_transform,
                                                                instance.transform, 0.25F, false);
    CY_CHECK_NEAR(blended.translation.x, 3.0F, 1e-5F);
}

CY_TEST_CASE("cameras and lights cross the boundary whole, with the entity as their identity") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());

    cy::ecs::ComponentTypeId camera_components[2] = {fixture.scene().world_transform,
                                                     fixture.render().camera};
    const auto camera_entity =
        fixture.world.create(cy::Span<const cy::ecs::ComponentTypeId>(camera_components, 2));
    CY_REQUIRE(camera_entity.has_value());
    Camera camera;
    camera.viewport = cy::render::ViewportRect{0, 0, 1920, 1080};
    CY_REQUIRE(fixture.world.set(*camera_entity, fixture.render().camera, camera).has_value());

    cy::ecs::ComponentTypeId light_components[2] = {fixture.scene().world_transform,
                                                    fixture.render().light_source};
    const auto light_entity =
        fixture.world.create(cy::Span<const cy::ecs::ComponentTypeId>(light_components, 2));
    CY_REQUIRE(light_entity.has_value());
    LightSource light;
    light.kind = cy::render::LightKind::Spot;
    light.intensity = 1500.0F;
    CY_REQUIRE(fixture.world.set(*light_entity, fixture.render().light_source, light).has_value());

    CY_REQUIRE(fixture.commit(1));
    CY_REQUIRE_EQ(fixture.published().cameras.size(), 1U);
    CY_REQUIRE_EQ(fixture.published().lights.size(), 1U);
    CY_CHECK_EQ(fixture.published().cameras[0].stable_id, camera_entity->bits());
    CY_CHECK_EQ(fixture.published().cameras[0].viewport.width, 1920U);
    // A camera with no history identity of its own falls back to the entity's, so two views never
    // share the history of "whoever also left it zero".
    CY_CHECK_EQ(fixture.published().cameras[0].history_id, camera_entity->bits());
    CY_CHECK(fixture.published().lights[0].desc.kind == cy::render::LightKind::Spot);
    CY_CHECK_EQ(fixture.published().lights[0].desc.stable_id, light_entity->bits());
    CY_CHECK_NEAR(fixture.published().lights[0].desc.intensity, 1500.0F, 1e-3F);

    // Disabling either removes it from the next snapshot without destroying anything.
    auto* stored = fixture.world.get_mut<Camera>(*camera_entity, fixture.render().camera);
    CY_REQUIRE(stored != nullptr);
    stored->enabled = false;
    CY_REQUIRE(fixture.commit(2));
    CY_CHECK_EQ(fixture.published().cameras.size(), 0U);
    // Lights and cameras are carried whole every tick, so the light is still there.
    CY_CHECK_EQ(fixture.published().lights.size(), 1U);
}

CY_TEST_CASE("the environment rides the snapshot") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    cy::render::EnvironmentSettings environment;
    environment.ambient_intensity = 0.25F;
    environment.fog_enabled = true;
    fixture.extractor.set_environment(environment);

    CY_REQUIRE(fixture.commit(1));
    CY_CHECK_NEAR(fixture.published().environment.ambient_intensity, 0.25F, 1e-6F);
    CY_CHECK(fixture.published().environment.fog_enabled);
}

CY_TEST_CASE("the buffer being written is never the buffer being read") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.spawn(cy::Vec3{0.0F, 0.0F, 0.0F}).valid());
    CY_REQUIRE(fixture.commit(1));

    const cy::render::RenderSnapshot* held = fixture.buffer.readable();
    CY_REQUIRE(held != nullptr);
    const cy::usize held_size = held->changed.size();

    // A renderer holds its snapshot for the whole frame. The next commit fills the other buffer, so
    // what it holds does not change underneath it.
    CY_REQUIRE(fixture.spawn(cy::Vec3{1.0F, 0.0F, 0.0F}).valid());
    CY_REQUIRE(fixture.commit(2));
    CY_CHECK_EQ(held->changed.size(), held_size);
    CY_CHECK_EQ(fixture.buffer.published_count(), 2ULL);
}

CY_TEST_CASE("resetting re-publishes the world rather than diffing against a dead one") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.spawn(cy::Vec3{0.0F, 0.0F, 0.0F}).valid());
    CY_REQUIRE(fixture.commit(1));
    CY_REQUIRE(fixture.commit(2));
    CY_CHECK_EQ(fixture.published().changed.size(), 0U);

    fixture.extractor.reset();
    CY_CHECK_EQ(fixture.extractor.published().size(), 0U);
    CY_REQUIRE(fixture.commit(3));
    CY_CHECK_EQ(fixture.published().changed.size(), 1U);
    // No removals: the consumer is being torn down too, and a list naming instances nobody holds is
    // work for no reader.
    CY_CHECK_EQ(fixture.published().removed.size(), 0U);
}

CY_TEST_CASE("two identical worlds produce identical snapshots") {
    // design.md §6, at the boundary rather than at the draw call. If extraction order came from a
    // hash map or from publication order, this is where it would show — and it would show as a
    // golden image that reproduces on one machine and not the next.
    Fixture first;
    Fixture second;
    CY_REQUIRE(first.start());
    CY_REQUIRE(second.start());

    for (u32 index = 0; index < 16; ++index) {
        const cy::Vec3 position{static_cast<f32>(index), 0.0F, static_cast<f32>(index) * 0.5F};
        CY_REQUIRE(first.spawn(position, (index % 3U) == 0U).valid());
        CY_REQUIRE(second.spawn(position, (index % 3U) == 0U).valid());
    }
    CY_REQUIRE(first.commit(1));
    CY_REQUIRE(second.commit(1));

    CY_REQUIRE_EQ(first.published().changed.size(), second.published().changed.size());
    for (cy::usize index = 0; index < first.published().changed.size(); ++index) {
        const cy::render::InstanceSnapshot& a = first.published().changed[index];
        const cy::render::InstanceSnapshot& b = second.published().changed[index];
        CY_CHECK_EQ(a.stable_id, b.stable_id);
        CY_CHECK_EQ(a.flags, b.flags);
        CY_CHECK_EQ(a.transform.translation.x, b.transform.translation.x);
        CY_CHECK_EQ(a.transform.translation.z, b.transform.translation.z);
    }

    // And the published set is in ascending stable-id order, which is what makes the removal list
    // ordered without a sort.
    const cy::Span<const u64> published = first.extractor.published();
    for (cy::usize index = 1; index < published.size(); ++index) {
        CY_CHECK_LT(published[index - 1U], published[index]);
    }
}

CY_TEST_CASE("extraction over a world with no components registered is refused") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    cy::render::SnapshotBuffer buffer(allocator());
    RenderComponents none;
    cy::scene::SceneComponents no_scene;
    SnapshotExtractor extractor(allocator(), world, none, no_scene, buffer);

    cy::determinism::CommitRecord record;
    const cy::Status extracted = extractor.on_commit(record);
    CY_REQUIRE_FALSE(extracted.has_value());
    CY_CHECK(extracted.error().code == cy::ErrorCode::InvalidArgument);
    // And nothing was published: a refusal that had already swapped the buffers would leave the
    // renderer holding an empty snapshot it would treat as an empty world.
    CY_CHECK(buffer.readable() == nullptr);
}
