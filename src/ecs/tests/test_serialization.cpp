// World serialization and snapshots. Task 2.10.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/query.h>
#include <cy/ecs/snapshot.h>
#include <cy/ecs/world.h>

#include "fixtures.h"

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Ecs);
}

/// A world with a few of everything: two archetypes, a shared value, a sparse entry, a buffer that
/// has spilled, a hierarchy, and a cross-entity reference.
struct Populated {
    cy::ecs::test::Components ids;
    cy::ecs::Entity anchor;
    cy::ecs::Entity follower;
    cy::ecs::Entity parent;
    cy::ecs::Entity child;
};

[[nodiscard]] cy::Expected<Populated, cy::Error> populate(cy::ecs::World& world) noexcept {
    Populated out;
    const auto ids = cy::ecs::test::register_all(world);
    if (!ids) {
        return cy::make_unexpected(ids.error());
    }
    out.ids = *ids;

    auto anchor = world.create();
    auto follower = world.create();
    auto parent = world.create();
    auto child = world.create();
    if (!anchor || !follower || !parent || !child) {
        return cy::fail(cy::ErrorCode::Internal, "could not create the fixture entities");
    }
    out.anchor = *anchor;
    out.follower = *follower;
    out.parent = *parent;
    out.child = *child;

    if (!world.add(out.anchor, ids->position) || !world.add(out.follower, ids->position) ||
        !world.add(out.follower, ids->velocity) || !world.add(out.follower, ids->target) ||
        !world.add(out.anchor, ids->waypoints)) {
        return cy::fail(cy::ErrorCode::Internal, "could not add the fixture components");
    }
    if (!world.set(out.anchor, ids->position, cy::ecs::test::Position{1.0F, 2.0F, 3.0F}) ||
        !world.set(out.follower, ids->position, cy::ecs::test::Position{4.0F, 5.0F, 6.0F}) ||
        !world.set(out.follower, ids->target, cy::ecs::test::Target{out.anchor})) {
        return cy::fail(cy::ErrorCode::Internal, "could not set the fixture values");
    }

    auto waypoints = world.buffer<cy::ecs::test::Waypoint>(out.anchor, ids->waypoints);
    if (!waypoints) {
        return cy::make_unexpected(waypoints.error());
    }
    for (cy::u32 index = 0; index < 9; ++index) {
        if (!waypoints->push_back(cy::ecs::test::Waypoint{static_cast<cy::f32>(index),
                                                          static_cast<cy::f32>(index)})) {
            return cy::fail(cy::ErrorCode::Internal, "could not fill the waypoint buffer");
        }
    }

    const cy::ecs::test::Material stone{11};
    auto interned = world.intern_shared(ids->material, &stone);
    if (!interned || !world.set_shared(out.anchor, ids->material, *interned)) {
        return cy::fail(cy::ErrorCode::Internal, "could not set the shared value");
    }

    const cy::ecs::test::Selected selected{77};
    if (!world.set_sparse(out.follower, ids->selected, &selected)) {
        return cy::fail(cy::ErrorCode::Internal, "could not set the sparse value");
    }
    if (!world.set_parent(out.child, out.parent)) {
        return cy::fail(cy::ErrorCode::Internal, "could not build the hierarchy");
    }
    return out;
}

}  // namespace

CY_TEST_CASE("a snapshot restores the world it was taken from, byte for byte in effect") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto fixture = populate(world);
    CY_REQUIRE(fixture.has_value());

    cy::ecs::Snapshot snapshot(allocator());
    CY_REQUIRE(snapshot.capture(world).has_value());
    CY_CHECK_EQ(snapshot.entity_count(), world.entity_count());
    CY_CHECK_GT(snapshot.bytes(), 0u);

    // Play mode: mutate everything the snapshot covers.
    CY_REQUIRE(world
                   .set(fixture->anchor, fixture->ids.position,
                        cy::ecs::test::Position{99.0F, 99.0F, 99.0F})
                   .has_value());
    CY_REQUIRE(world.destroy(fixture->follower).has_value());
    CY_REQUIRE(world.set_parent(fixture->child, cy::ecs::kNoEntity).has_value());
    auto extra = world.create();
    CY_REQUIRE(extra.has_value());
    CY_REQUIRE(world.add(*extra, fixture->ids.velocity).has_value());
    CY_CHECK_FALSE(world.is_alive(fixture->follower));
    CY_CHECK_EQ(cy::ecs::test::value_of<cy::ecs::test::Position>(world, fixture->anchor,
                                                                 fixture->ids.position)
                    .x,
                99.0F);

    // Exiting play mode restores from the snapshot, with no reliance on anything undoing its own
    // mutations.
    CY_REQUIRE(snapshot.restore(world).has_value());
    CY_CHECK_EQ(world.entity_count(), snapshot.entity_count());
    CY_CHECK(world.is_alive(fixture->anchor));
    CY_CHECK(world.is_alive(fixture->follower));
    CY_CHECK_FALSE(world.is_alive(*extra));

    const auto* position =
        world.get<cy::ecs::test::Position>(fixture->anchor, fixture->ids.position);
    CY_REQUIRE(position != nullptr);
    CY_CHECK_EQ(position->x, 1.0F);

    const auto* target = world.get<cy::ecs::test::Target>(fixture->follower, fixture->ids.target);
    CY_REQUIRE(target != nullptr);
    CY_CHECK_EQ(target->entity, fixture->anchor);

    // The buffer's heap spill is deep-copied by the snapshot, so restoring does not hand two owners
    // one block.
    auto waypoints = world.buffer<cy::ecs::test::Waypoint>(fixture->anchor, fixture->ids.waypoints);
    CY_REQUIRE(waypoints.has_value());
    CY_REQUIRE_EQ(waypoints->size(), 9u);
    CY_CHECK_EQ((*waypoints)[8].x, 8.0F);

    const auto* selected = static_cast<const cy::ecs::test::Selected*>(
        world.get_sparse(fixture->follower, fixture->ids.selected));
    CY_REQUIRE(selected != nullptr);
    CY_CHECK_EQ(selected->tick, 77u);

    CY_CHECK_EQ(world.parent_of(fixture->child), fixture->parent);
    const auto shared = world.shared_of(fixture->anchor, fixture->ids.material);
    CY_REQUIRE(shared.has_value());
    const auto* material = static_cast<const cy::ecs::test::Material*>(
        world.shared_value(fixture->ids.material, *shared));
    CY_REQUIRE(material != nullptr);
    CY_CHECK_EQ(material->id, 11u);
}

CY_TEST_CASE("a serialized world round-trips into a second world with its references intact") {
    cy::ecs::World source(allocator());
    CY_REQUIRE(source.initialize().has_value());
    const auto fixture = populate(source);
    CY_REQUIRE(fixture.has_value());

    cy::Array<cy::u8> stream(allocator());
    CY_REQUIRE(cy::ecs::serialize(source, stream).has_value());
    CY_CHECK_GT(stream.size(), 0u);

    cy::ecs::World target(allocator());
    CY_REQUIRE(target.initialize().has_value());
    const auto target_ids = cy::ecs::test::register_all(target);
    CY_REQUIRE(target_ids.has_value());

    cy::Array<cy::ecs::Entity> loaded(allocator());
    CY_REQUIRE(cy::ecs::deserialize(target, stream.span(), loaded).has_value());
    CY_CHECK_EQ(loaded.size(), source.entity_count());
    CY_CHECK_EQ(target.entity_count(), source.entity_count());

    // Find the reloaded follower by its component set, then check that its reference resolves to
    // the reloaded anchor — which is a different entity id in a different world.
    cy::ecs::Entity reloaded_follower;
    cy::ecs::Entity reloaded_anchor;
    for (const cy::ecs::Entity entity : loaded) {
        if (target.has(entity, target_ids->target)) {
            reloaded_follower = entity;
        }
        if (target.has(entity, target_ids->waypoints)) {
            reloaded_anchor = entity;
        }
    }
    CY_REQUIRE(reloaded_follower.valid());
    CY_REQUIRE(reloaded_anchor.valid());
    CY_CHECK_NE(reloaded_anchor, fixture->anchor);

    const auto* target_component =
        target.get<cy::ecs::test::Target>(reloaded_follower, target_ids->target);
    CY_REQUIRE(target_component != nullptr);
    CY_CHECK_EQ(target_component->entity, reloaded_anchor);
    CY_CHECK(target.is_alive(target_component->entity));

    const auto* position =
        target.get<cy::ecs::test::Position>(reloaded_anchor, target_ids->position);
    CY_REQUIRE(position != nullptr);
    CY_CHECK_EQ(position->x, 1.0F);

    auto waypoints = target.buffer<cy::ecs::test::Waypoint>(reloaded_anchor, target_ids->waypoints);
    CY_REQUIRE(waypoints.has_value());
    CY_CHECK_EQ(waypoints->size(), 9u);

    // The hierarchy is entity references in a buffer component and in Parent; both were remapped.
    cy::ecs::Entity reloaded_child;
    for (const cy::ecs::Entity entity : loaded) {
        if (target.parent_of(entity).valid()) {
            reloaded_child = entity;
        }
    }
    CY_REQUIRE(reloaded_child.valid());
    const cy::ecs::Entity reloaded_parent = target.parent_of(reloaded_child);
    CY_CHECK(target.is_alive(reloaded_parent));
    CY_REQUIRE_EQ(target.children_of(reloaded_parent).size(), 1u);
    CY_CHECK_EQ(target.children_of(reloaded_parent)[0], reloaded_child);
}

CY_TEST_CASE("a filtered write keeps the subset and nulls the references that leave it") {
    cy::ecs::World source(allocator());
    CY_REQUIRE(source.initialize().has_value());
    const auto fixture = populate(source);
    CY_REQUIRE(fixture.has_value());

    // Only the follower. Its Target names the anchor, which is not in the subset.
    const cy::ecs::Entity subset[] = {fixture->follower};
    cy::Array<cy::u8> stream(allocator());
    CY_REQUIRE(
        cy::ecs::serialize(source, stream, cy::Span<const cy::ecs::Entity>(subset, 1)).has_value());

    cy::ecs::World target(allocator());
    CY_REQUIRE(target.initialize().has_value());
    const auto target_ids = cy::ecs::test::register_all(target);
    CY_REQUIRE(target_ids.has_value());
    cy::Array<cy::ecs::Entity> loaded(allocator());
    CY_REQUIRE(cy::ecs::deserialize(target, stream.span(), loaded).has_value());

    CY_REQUIRE_EQ(loaded.size(), 1u);
    const auto* reference = target.get<cy::ecs::test::Target>(loaded[0], target_ids->target);
    CY_REQUIRE(reference != nullptr);
    // Explicit at write time rather than resolving later to whatever occupies that id.
    CY_CHECK_FALSE(reference->entity.valid());
    const auto* position = target.get<cy::ecs::test::Position>(loaded[0], target_ids->position);
    CY_REQUIRE(position != nullptr);
    CY_CHECK_EQ(position->x, 4.0F);
}

CY_TEST_CASE(
    "a stream naming an unregistered component is refused rather than loaded with a hole") {
    cy::ecs::World source(allocator());
    CY_REQUIRE(source.initialize().has_value());
    const auto fixture = populate(source);
    CY_REQUIRE(fixture.has_value());
    cy::Array<cy::u8> stream(allocator());
    CY_REQUIRE(cy::ecs::serialize(source, stream).has_value());

    cy::ecs::World bare(allocator());
    CY_REQUIRE(bare.initialize().has_value());
    cy::Array<cy::ecs::Entity> loaded(allocator());
    const auto refused = cy::ecs::deserialize(bare, stream.span(), loaded);
    CY_CHECK_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, cy::ErrorCode::NotFound);

    // Truncation and a foreign magic number are both reported at the read that ran off the end.
    cy::Array<cy::ecs::Entity> nothing(allocator());
    const cy::u8 garbage[] = {1, 2, 3, 4, 5, 6, 7, 8};
    CY_CHECK_FALSE(
        cy::ecs::deserialize(bare, cy::Span<const cy::u8>(garbage, 8), nothing).has_value());
    CY_CHECK_FALSE(cy::ecs::deserialize(bare, stream.span().subspan(0, 12), nothing).has_value());
}
