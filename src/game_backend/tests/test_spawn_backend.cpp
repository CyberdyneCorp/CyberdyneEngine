// SPDX-License-Identifier: MIT
// `integration.game_backend_spawn`: ABI 1.3's `spawn_*` entries over a real `SceneTree`, through
// the interface table, with the `SpawnAdapter` bound on a host. `add-swift-game-api`.
//
// OWNER: implementer C. The claims design.md asks this suite for: the same calls give IDENTICAL
// entities on two runs, a batch is ALL OR NOTHING, every structural call BUMPS THE EPOCH, and
// destruction runs the destroy callbacks CHILD FIRST.
//
// The prefab is three nodes — Tank, its Turret, the Turret's Barrel — each with a `Probe`
// behaviour that logs its node's name on create and on destroy.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/game_backend/spawn_backend.h>
#include <cy/scene/behaviour.h>
#include <cy/scene/tree.h>
#include <cy/test/test.h>

#include <string>
#include <vector>

namespace {

using namespace cy;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

// --- The probe behaviour -------------------------------------------------------------------------

std::vector<std::string> g_created;    // NOLINT: test-only log, reset by every fixture
std::vector<std::string> g_destroyed;  // NOLINT
/// When non-zero, the Nth `Tank` created destroys itself in its own create callback — the one way
/// to make a batch fail part-way with no test hook in the adapter.
int g_sabotage_tank = 0;  // NOLINT
int g_tanks = 0;          // NOLINT

void probe_create(const scene::BehaviourContext& context) noexcept {
    const std::string name(context.node.name().text());
    g_created.push_back(name);
    if (name.starts_with("Tank") && ++g_tanks == g_sabotage_tank) {
        (void)context.tree->destroy_node(context.node);
    }
}

void probe_destroy(const scene::BehaviourContext& context) noexcept {
    g_destroyed.emplace_back(context.node.name().text());
}

// --- The prefab ----------------------------------------------------------------------------------

scene::NodeDesc node(const char* name, u32 parent, Vec3 at) noexcept {
    scene::NodeDesc desc;
    desc.name = Name::intern(name);
    desc.parent = parent;
    desc.local_transform = Transform::from_translation(at);
    desc.behaviour = Name::intern("Probe");
    return desc;
}

const scene::NodeDesc kTankNodes[] = {
    node("Tank", scene::NodeDesc::kNoParent, Vec3{0.0F, 1.0F, 0.0F}),
    node("Turret", 0, Vec3{0.0F, 0.5F, 0.0F}),
    node("Barrel", 1, Vec3{0.0F, 0.0F, -2.0F}),
};

scene::SceneDescription tank() noexcept {
    return scene::SceneDescription{Name::intern("units/tank"),
                                   Span<const scene::NodeDesc>(kTankNodes)};
}

Status load_tank(void* user, const char* /*path*/, scene::SceneDescription& out) noexcept {
    ++*static_cast<int*>(user);
    out = tank();
    return ok();
}

/// A world, its scene tree, its ABI binding, and the adapter bound on a host, in no phase.
struct SpawnWorld {
    ecs::World world{allocator()};
    scene::SceneTree tree{world};
    abi::World binding{system_allocator(MemoryDomain::Scripting), world};
    abi::Host host{system_allocator(MemoryDomain::Scripting)};
    game_backend::SpawnAdapter adapter{tree, allocator()};
    CyPrefab prefab = CY_PREFAB_NULL;

    SpawnWorld() {
        g_created.clear();
        g_destroyed.clear();
        g_sabotage_tank = 0;
        g_tanks = 0;
        CY_REQUIRE(world.initialize().has_value());
        CY_REQUIRE(tree.initialize().has_value());
        scene::BehaviourDesc probe;
        probe.name = "Probe";
        probe.on_create = &probe_create;
        probe.on_destroy = &probe_destroy;
        CY_REQUIRE(tree.behaviours().add(world, probe).has_value());
        CY_REQUIRE(adapter.add_prefab("units/tank", tank()).has_value());
        host.bind_world(&binding);
        game_backend::bind(host, &adapter);
        CY_REQUIRE_EQ(table().spawn_resolve(&host, "units/tank", &prefab), CY_RESULT_OK);
    }

    CyEntity spawn(float x, CyEntity parent = CY_ENTITY_NULL) {
        CySpawnParams params{};
        params.struct_size = sizeof(CySpawnParams);
        params.parent = parent;
        params.pose.position[0] = x;
        CyEntity root = CY_ENTITY_NULL;
        CY_REQUIRE_EQ(table().spawn_instantiate(&host, prefab, &params, &root), CY_RESULT_OK);
        return root;
    }

    [[nodiscard]] scene::Node node_of(CyEntity entity) { return tree.node(abi::from_abi(entity)); }
};

/// One scripted fixed step's worth of spawning, recording every entity it produced.
std::vector<CyEntity> scripted_run() {
    SpawnWorld fixture;
    fixture.host.game.clock.phase = CY_PHASE_FIXED_UPDATE;
    std::vector<CyEntity> produced;
    produced.push_back(fixture.spawn(1.0F));
    const CyPose poses[3] = {};
    CyEntity roots[3] = {};
    CY_REQUIRE_EQ(
        table().spawn_instantiate_many(&fixture.host, fixture.prefab, produced[0], poses, 3, roots),
        CY_RESULT_OK);
    produced.insert(produced.end(), roots, roots + 3);
    CY_REQUIRE_EQ(table().spawn_destroy(&fixture.host, roots[1]), CY_RESULT_OK);
    produced.push_back(fixture.spawn(2.0F));
    const std::vector<CyEntity> roots_made = produced;
    for (const CyEntity root : roots_made) {
        const scene::Node turret = fixture.node_of(root).child(0);
        if (turret.valid()) {
            produced.push_back(abi::to_abi(turret.entity()));
        }
    }
    return produced;
}

}  // namespace

CY_TEST_CASE(
    "spawn backend: an instance is the whole prefab, placed, with its behaviours created") {
    SpawnWorld fixture;
    const CyEntity root = fixture.spawn(10.0F);
    const scene::Node tank_node = fixture.node_of(root);
    CY_REQUIRE(tank_node.valid());
    // No parent: attached under the tree's root, so it is in the tree.
    CY_CHECK_EQ(tank_node.parent().entity(), fixture.tree.root().entity());
    CY_CHECK_EQ(tank_node.child_count(), 1U);
    CY_CHECK_EQ(tank_node.child(0).child_count(), 1U);
    // The pose composes onto the authored root transform: (10, 0, 0) then (0, 1, 0).
    const Transform local = tank_node.local_transform();
    CY_CHECK_EQ(local.translation.x, 10.0F);
    CY_CHECK_EQ(local.translation.y, 1.0F);
    CY_CHECK_EQ(local.scale.x, 1.0F);  // an all-zero scale is one
    // Every node's behaviour `create` has run before the call returned.
    CY_CHECK_EQ(g_created, (std::vector<std::string>{"Tank", "Turret", "Barrel"}));
}

CY_TEST_CASE("spawn backend: the same calls give identical entities on two runs") {
    const std::vector<CyEntity> first = scripted_run();
    const std::vector<CyEntity> second = scripted_run();
    CY_REQUIRE_EQ(first.size(), second.size());
    CY_CHECK_EQ(first, second);
    CY_CHECK_GT(first.size(), 5U);
}

CY_TEST_CASE("spawn backend: a batch that fails part-way leaves no instance of it behind") {
    SpawnWorld fixture;
    const CyEntity parent = fixture.spawn(0.0F);
    const u32 children_before = fixture.node_of(parent).child_count();
    const u64 entities_before = fixture.world.entity_count();
    const u64 epoch_before = fixture.binding.epoch;

    g_tanks = 0;
    g_sabotage_tank = 3;  // the third instance's root destroys itself mid-creation
    const CyPose poses[4] = {};
    CyEntity roots[4] = {1, 2, 3, 4};
    CY_CHECK_NE(
        table().spawn_instantiate_many(&fixture.host, fixture.prefab, parent, poses, 4, roots),
        CY_RESULT_OK);
    CY_CHECK_EQ(g_tanks, 3);  // two whole instances existed before the third failed
    CY_CHECK_EQ(fixture.node_of(parent).child_count(), children_before);
    CY_CHECK_EQ(fixture.world.entity_count(), entities_before);
    CY_CHECK_EQ(roots[0], 1U);  // the caller's buffer is untouched
    CY_CHECK_EQ(fixture.binding.epoch, epoch_before);
    abi::clear_last_error();
}

CY_TEST_CASE("spawn backend: every structural call bumps the epoch, and a failure does not") {
    SpawnWorld fixture;
    u64 epoch = fixture.binding.epoch;
    const CyEntity root = fixture.spawn(0.0F);
    CY_CHECK_EQ(fixture.binding.epoch, ++epoch);

    const CyPose poses[2] = {};
    CyEntity roots[2] = {};
    CY_REQUIRE_EQ(
        table().spawn_instantiate_many(&fixture.host, fixture.prefab, root, poses, 2, roots),
        CY_RESULT_OK);
    CY_CHECK_EQ(fixture.binding.epoch, ++epoch);

    CY_REQUIRE_EQ(table().spawn_destroy(&fixture.host, roots[0]), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.binding.epoch, ++epoch);

    CY_CHECK_EQ(table().spawn_destroy(&fixture.host, roots[0]), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(fixture.binding.epoch, epoch);
    abi::clear_last_error();
}

CY_TEST_CASE("spawn backend: destroy runs every destroy callback, children first") {
    SpawnWorld fixture;
    const CyEntity root = fixture.spawn(0.0F);
    CY_REQUIRE_EQ(table().spawn_destroy(&fixture.host, root), CY_RESULT_OK);
    CY_CHECK_EQ(g_destroyed, (std::vector<std::string>{"Barrel", "Turret", "Tank"}));
    CY_CHECK_FALSE(fixture.node_of(root).valid());
}

CY_TEST_CASE("spawn backend: a dead parent, a stale prefab and an unknown path are NOT_FOUND") {
    SpawnWorld fixture;
    const CyEntity doomed = fixture.spawn(0.0F);
    CY_REQUIRE_EQ(table().spawn_destroy(&fixture.host, doomed), CY_RESULT_OK);

    CySpawnParams params{};
    params.struct_size = sizeof(CySpawnParams);
    params.parent = doomed;
    CyEntity root = CY_ENTITY_NULL;
    CY_CHECK_EQ(table().spawn_instantiate(&fixture.host, fixture.prefab, &params, &root),
                CY_RESULT_NOT_FOUND);

    CyPrefab prefab = CY_PREFAB_NULL;
    CY_CHECK_EQ(table().spawn_resolve(&fixture.host, "units/never-registered", &prefab),
                CY_RESULT_NOT_FOUND);

    // The level that issued the handle is gone: the handle is stale, even if the path comes back.
    fixture.adapter.clear();
    CY_REQUIRE(fixture.adapter.add_prefab("units/tank", tank()).has_value());
    params.parent = CY_ENTITY_NULL;
    CY_CHECK_EQ(table().spawn_instantiate(&fixture.host, fixture.prefab, &params, &root),
                CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().spawn_destroy(&fixture.host, abi::to_abi(fixture.tree.root().entity())),
                CY_RESULT_INVALID_ARGUMENT);
    abi::clear_last_error();
}

CY_TEST_CASE("spawn backend: a loadable prefab loads only in no phase, then resolves anywhere") {
    SpawnWorld fixture;
    int loads = 0;
    CY_REQUIRE(fixture.adapter.add_loadable("units/scout", &load_tank, &loads).has_value());

    CyPrefab prefab = CY_PREFAB_NULL;
    fixture.host.game.clock.phase = CY_PHASE_FIXED_UPDATE;
    CY_CHECK_EQ(table().spawn_resolve(&fixture.host, "units/scout", &prefab),
                CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(loads, 0);

    fixture.host.game.clock.phase = CY_PHASE_NONE;
    CY_REQUIRE_EQ(table().spawn_resolve(&fixture.host, "units/scout", &prefab), CY_RESULT_OK);
    CY_CHECK_EQ(loads, 1);

    fixture.host.game.clock.phase = CY_PHASE_FIXED_UPDATE;
    CyPrefab again = CY_PREFAB_NULL;
    CY_REQUIRE_EQ(table().spawn_resolve(&fixture.host, "units/scout", &again), CY_RESULT_OK);
    CY_CHECK_EQ(again, prefab);
    CY_CHECK_EQ(loads, 1);
    abi::clear_last_error();
}

CY_TEST_CASE("spawn backend: a prefab that is not one rooted tree in order is refused") {
    SpawnWorld fixture;
    const scene::NodeDesc orphan[] = {node("A", scene::NodeDesc::kNoParent, Vec3{}),
                                      node("B", scene::NodeDesc::kNoParent, Vec3{})};
    CY_CHECK_FALSE(fixture.adapter
                       .add_prefab("bad/two-roots",
                                   scene::SceneDescription{Name::intern("bad"),
                                                           Span<const scene::NodeDesc>(orphan)})
                       .has_value());
    CY_CHECK_FALSE(fixture.adapter.add_prefab("units/tank", tank()).has_value());  // taken
}
