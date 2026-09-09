// Spawning. M8.a task 5.1.
//
// `gameplay-framework`'s "Spawning" requirement, sentence by sentence, and where each is checked:
//
//   "a spawn request — an entity template, an owner, a team, and a context — and returning a
//    result, applying declared spawn rules to select a location"
//        -> `spawn()` returns where it went, which is the policy's answer and not the request's
//   "Batch spawning SHALL be first-class"
//        -> `spawn_many` is one call for many. HOW first-class is measured rather than
//           claimed: the case names the cost of the half that is not batched yet
//   "reservation: a location may be reserved while dependencies stream, so two simultaneous
//    requests do not select the same point"
//        -> a reserved point is skipped by `NearestFree` and refused by `SpawnPoint`
//   "Spawn points SHALL be representable as spatial metadata; instantiating an entity per spawn
//    point SHALL NOT be required"
//        -> registering four points creates no entities, which the case asserts by counting them
//
// AND THE ONE THAT IS NOT IN THE REQUIREMENT: a policy this build cannot honour is REFUSED by name
// rather than silently falling back to the request's own placement. A designer who chose
// "navigation-reachable" and got "wherever you happened to put it" has been lied to.

#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/play/spawn.h>
#include <cy/scene/tree.h>
#include <cy/test/test.h>

using cy::f32;
using cy::u32;
using cy::u64;
using namespace cy::gameplay;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

struct Fixture {
    Fixture() noexcept : world(allocator()), tree(world), spawns(tree, allocator()) {
        started = world.initialize().has_value() && tree.initialize().has_value();
    }

    [[nodiscard]] cy::Vec3 position_of(cy::ecs::Entity entity) const noexcept {
        const auto* local =
            world.get<cy::scene::LocalTransform>(entity, tree.components().local_transform);
        return local == nullptr ? cy::Vec3{} : local->value.translation;
    }

    cy::ecs::World world;
    cy::scene::SceneTree tree;
    SpawnService spawns;
    bool started = false;
};

[[nodiscard]] SpawnRequest at(f32 x, f32 y, f32 z) {
    SpawnRequest request;
    request.name = cy::Name::intern("Spawned");
    request.placement = cy::Transform::from_translation(cy::Vec3{x, y, z});
    return request;
}

}  // namespace

CY_TEST_CASE("a spawn puts an entity where the request said") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);

    const auto spawned = fixture.spawns.spawn(at(1.0F, 2.0F, 3.0F));
    CY_REQUIRE(spawned.has_value());
    CY_CHECK(spawned->entity.valid());
    CY_CHECK_EQ(spawned->point, kNoSpawnPoint);
    const cy::Vec3 where = fixture.position_of(spawned->entity);
    CY_CHECK_NEAR(where.x, 1.0F, 0.0001);
    CY_CHECK_NEAR(where.y, 2.0F, 0.0001);
    CY_CHECK_NEAR(where.z, 3.0F, 0.0001);
    CY_CHECK_EQ(fixture.spawns.statistics().spawned, 1U);
}

CY_TEST_CASE("spawn points are metadata and instantiate nothing") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const u32 before = fixture.world.entity_count();

    for (u32 index = 0; index < 4; ++index) {
        const auto added = fixture.spawns.add_point(
            cy::Name::intern("Point"),
            cy::Transform::from_translation(cy::Vec3{static_cast<f32>(index), 0.0F, 0.0F}));
        CY_REQUIRE(added.has_value());
    }
    CY_CHECK_EQ(fixture.spawns.point_count(), 4U);
    // FOUR POINTS AND NO ENTITIES. A spawn point that was an entity would be four archetype rows
    // every gameplay query steps over, for a level with four hundred of them.
    CY_CHECK_EQ(fixture.world.entity_count(), before);
}

CY_TEST_CASE("a named point is where a spawn goes, and an unknown one is refused") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    CY_REQUIRE(fixture.spawns
                   .add_point(cy::Name::intern("Red"),
                              cy::Transform::from_translation(cy::Vec3{-10.0F, 0.0F, 0.0F}))
                   .has_value());

    SpawnRequest request = at(999.0F, 999.0F, 999.0F);
    request.policy = SpawnPolicy::SpawnPoint;
    request.point = cy::Name::intern("Red");
    const auto spawned = fixture.spawns.spawn(request);
    CY_REQUIRE(spawned.has_value());
    // THE POLICY'S ANSWER, NOT THE REQUEST'S. A service that used the placement anyway would make
    // every policy decorative.
    CY_CHECK_NEAR(fixture.position_of(spawned->entity).x, -10.0F, 0.0001);

    request.point = cy::Name::intern("Blue");
    const auto missing = fixture.spawns.spawn(request);
    CY_REQUIRE_FALSE(missing.has_value());
    CY_CHECK(missing.error().code == cy::ErrorCode::NotFound);
    CY_CHECK_EQ(fixture.spawns.statistics().refused, 1U);
}

CY_TEST_CASE("two requests do not select the same point") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const auto first = fixture.spawns.add_point(
        cy::Name::intern("A"), cy::Transform::from_translation(cy::Vec3{1.0F, 0.0F, 0.0F}));
    const auto second = fixture.spawns.add_point(
        cy::Name::intern("B"), cy::Transform::from_translation(cy::Vec3{2.0F, 0.0F, 0.0F}));
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());

    SpawnRequest request = at(0.0F, 0.0F, 0.0F);
    request.policy = SpawnPolicy::NearestFree;

    // The first takes A and occupies it.
    const auto one = fixture.spawns.spawn(request, 0);
    CY_REQUIRE(one.has_value());
    CY_CHECK_EQ(one->point, *first);

    // Reserving B while its dependencies stream leaves nothing free.
    CY_REQUIRE(fixture.spawns.reserve(*second, 0, 100).has_value());
    const auto refused = fixture.spawns.spawn(request, 10);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::Unavailable);

    // A second reservation of a held point is refused rather than silently taking it over.
    CY_CHECK_FALSE(fixture.spawns.reserve(*second, 10, 200).has_value());

    // Past the reservation, B is free again.
    const auto later = fixture.spawns.spawn(request, 101);
    CY_REQUIRE(later.has_value());
    CY_CHECK_EQ(later->point, *second);
}

CY_TEST_CASE("a batch is one call and every instance lands") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);

    // EIGHT, AND THE NUMBER IS THE SUBJECT OF THIS COMMENT RATHER THAN AN ARBITRARY SMALL ONE.
    //
    // `SceneTree::create_node` makes a name unique among its siblings by scanning them, so spawning
    // N instances of ONE name costs O(N^2) name comparisons. That is the half of batch spawning
    // `spawn_many`'s own documentation says is not batched yet: the entity creation is one
    // operation, the naming is not.
    //
    // THIS CASE HELD SIXTY-FOUR AND WAS OVER THE UNIT BUDGET ON SEVEN RUNS IN EIGHT — 1.25 to 2.02
    // ms of CPU against 1.000 ms, standalone, on an idle host, and 0.865 ms on the one run where
    // the governor had clocked up. M8.a's closing gate measured `just test-unit` failing three runs
    // in six on it, which made `just test-all` and therefore `m0:test` a coin toss. The instrument
    // was right and the case was in the wrong suite: `tests/harness/src/budget.cpp` says so in the
    // failure itself — "the taxonomy in `testing-and-quality` places a test this expensive in the
    // next suite up — move it, or make it cheaper", and M7's gate moved three material cases for
    // exactly this reason.
    //
    // So both were done. What is checked HERE is `spawn_many`'s contract — one call, one batch
    // counted, every instance alive and placed — which eight instances check as well as sixty-four
    // do and which costs 0.03 ms rather than 1.8. What the cost IS, measured at two hundred with
    // the quadratic term visible, is `integration.gameplay_play`'s "batch spawning is one call and
    // a quadratic naming cost" — the suite whose budget can afford to state a number instead of
    // trimming the case until it stops measuring anything.
    cy::Array<cy::ecs::Entity> spawned(allocator());
    CY_REQUIRE(fixture.spawns.spawn_many(at(0.0F, 5.0F, 0.0F), 8, spawned).has_value());
    CY_CHECK_EQ(spawned.size(), cy::usize{8});
    CY_CHECK_EQ(fixture.spawns.statistics().batches, 1U);
    CY_CHECK_EQ(fixture.spawns.statistics().spawned, 8U);
    for (const cy::ecs::Entity entity : spawned) {
        CY_CHECK(fixture.world.is_alive(entity));
        CY_CHECK_NEAR(fixture.position_of(entity).y, 5.0F, 0.0001);
    }
}

CY_TEST_CASE("selecting answers the same question the spawn will") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    CY_REQUIRE(fixture.spawns
                   .add_point(cy::Name::intern("Only"),
                              cy::Transform::from_translation(cy::Vec3{7.0F, 0.0F, 0.0F}))
                   .has_value());
    SpawnRequest request = at(0.0F, 0.0F, 0.0F);
    request.policy = SpawnPolicy::NearestFree;

    // The interface greying out a button and the spawn itself ask one object, which is the argument
    // `CommandStream::validate` makes for returning reasons rather than a bool.
    const auto asked = fixture.spawns.select(request, 0);
    CY_REQUIRE(asked.has_value());
    CY_CHECK_FALSE(asked->entity.valid());          // asking creates nothing
    CY_CHECK_EQ(fixture.world.entity_count(), 1U);  // the tree's root, and no spawn

    const auto done = fixture.spawns.spawn(request, 0);
    CY_REQUIRE(done.has_value());
    CY_CHECK_EQ(done->point, asked->point);
    CY_CHECK_NEAR(done->placement.translation.x, asked->placement.translation.x, 0.0001);
}

CY_TEST_CASE("the policy names round-trip for a diagnostic") {
    CY_CHECK_EQ(std::string_view(spawn_policy_name(SpawnPolicy::ExactPosition)), "exact-position");
    CY_CHECK_EQ(std::string_view(spawn_policy_name(SpawnPolicy::SpawnPoint)), "spawn-point");
    CY_CHECK_EQ(std::string_view(spawn_policy_name(SpawnPolicy::NearestFree)), "nearest-free");
}
