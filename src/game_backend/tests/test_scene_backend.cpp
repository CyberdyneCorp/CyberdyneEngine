// SPDX-License-Identifier: MIT
// integration.game_backend_scene — `ScriptSceneBridge` over a real `SceneTree` and a real
// `BehaviourRuntime`, through the interface table. `add-swift-m12-gaps`.
//
// The claims: a script type the module registered becomes a scene behaviour of the same name; a
// script attached to a node is created at once and receives `enter_tree` parent first and `ready`
// child first at the next pump, `disable` and `enable` when its node's effective enablement
// changes, and `exit_tree` then `destroy` child first when the subtree is destroyed — the tree's
// own order, through the ABI 1.5 vtable entries; a script on a node that is not in the tree gets no
// tree callback; and `node_find` resolves relative and absolute paths against the same tree.
//
// The "module" is a vtable registered on the host directly, so this suite needs no shared library;
// the Swift side of the same path is integration.rts_api_sample.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/host.h>
#include <cy/abi/module.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/game_backend/scene_bridge.h>
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

// --- The probe script: each instance remembers its entity and logs "<callback><entity>" ----------

struct ProbeInstance {
    CyEntity entity = CY_ENTITY_NULL;
};

std::vector<std::string> g_log;  // NOLINT: test-only, reset by every fixture
bool g_refuse = false;           // NOLINT

void log(const char* what, CyInstance self) {
    g_log.push_back(std::string(what) + std::to_string(static_cast<ProbeInstance*>(self)->entity));
}

extern "C" CyInstance probe_create(CyEngine, CyEntity entity, void*) {
    if (g_refuse) {
        return nullptr;
    }
    auto* instance = new ProbeInstance{entity};  // NOLINT: freed by probe_destroy
    log("create:", instance);
    return instance;
}
extern "C" void probe_destroy(CyInstance self, void*) {
    log("destroy:", self);
    delete static_cast<ProbeInstance*>(self);  // NOLINT
}
extern "C" void probe_enter(CyInstance self, void*) {
    log("enter:", self);
}
extern "C" void probe_ready(CyInstance self, void*) {
    log("ready:", self);
}
extern "C" void probe_enable(CyInstance self, void*) {
    log("enable:", self);
}
extern "C" void probe_disable(CyInstance self, void*) {
    log("disable:", self);
}
extern "C" void probe_exit(CyInstance self, void*) {
    log("exit:", self);
}

CyBehaviourVTable probe_vtable() noexcept {
    CyBehaviourVTable vtable{};
    vtable.struct_size = sizeof(vtable);
    vtable.create = &probe_create;
    vtable.destroy = &probe_destroy;
    vtable.enter_tree = &probe_enter;
    vtable.ready = &probe_ready;
    vtable.enable = &probe_enable;
    vtable.disable = &probe_disable;
    vtable.exit_tree = &probe_exit;
    return vtable;
}

struct Level {
    ecs::World world{allocator()};
    scene::SceneTree tree{world};
    abi::World binding{allocator(), world};
    abi::Host host{allocator()};
    abi::BehaviourRuntime runtime{allocator(), host};
    game_backend::ScriptSceneBridge bridge{allocator(), tree, host, runtime};

    Level() {
        g_log.clear();
        g_refuse = false;
        CY_REQUIRE(world.initialize().has_value());
        CY_REQUIRE(tree.initialize().has_value());
        host.bind_world(&binding);
        CY_REQUIRE(host.register_behaviour("Probe", probe_vtable()).has_value());
        const Expected<u32, Error> synced = bridge.sync_types();
        CY_REQUIRE(synced.has_value());
        CY_REQUIRE_EQ(synced.value(), 1U);
        game_backend::bind_scene(host, &bridge);
    }

    ~Level() { game_backend::bind_scene(host, nullptr); }

    Level(const Level&) = delete;
    Level& operator=(const Level&) = delete;

    scene::Node make(const char* name, scene::Node parent) {
        const Expected<scene::Node, Error> made = tree.create_node(Name::intern(name), parent);
        CY_REQUIRE(made.has_value());
        return made.value();
    }

    [[nodiscard]] static std::string id(scene::Node node) {
        return std::to_string(abi::to_abi(node.entity()));
    }
};

}  // namespace

CY_TEST_CASE("a script on a node is created at once and gets the tree callbacks in tree order") {
    Level level;
    const scene::Node squad = level.make("Squad", level.tree.root());
    const scene::Node leader = level.make("Leader", squad);
    CY_REQUIRE(level.bridge.attach(squad, "Probe").has_value());
    CY_REQUIRE(level.bridge.attach(leader, "Probe").has_value());
    CY_CHECK_EQ(level.bridge.attached(), 2U);
    CY_CHECK_EQ(level.runtime.live_instances(), 2U);

    // Created synchronously; nothing tree-shaped until the pump.
    const std::string s = Level::id(squad);
    const std::string l = Level::id(leader);
    CY_CHECK(g_log == (std::vector<std::string>{"create:" + s, "create:" + l}));
    g_log.clear();

    // Both nodes were attached before the pump, so one subtree event carries them: enter parent
    // first, ready child first.
    CY_REQUIRE(level.tree.pump().has_value());
    CY_CHECK(g_log ==
             (std::vector<std::string>{"enter:" + s, "enter:" + l, "ready:" + l, "ready:" + s}));
    g_log.clear();
    // A second pump with nothing new dispatches nothing: ready is once per attachment.
    CY_REQUIRE(level.tree.pump().has_value());
    CY_CHECK(g_log.empty());

    // Disabling the parent disables both, observed at the pump after propagation.
    CY_REQUIRE(squad.set_enabled(false).has_value());
    CY_REQUIRE(level.tree.propagate().has_value());
    CY_REQUIRE(level.tree.pump().has_value());
    CY_CHECK(g_log == (std::vector<std::string>{"disable:" + s, "disable:" + l}));
    g_log.clear();
    CY_REQUIRE(squad.set_enabled(true).has_value());
    CY_REQUIRE(level.tree.propagate().has_value());
    CY_REQUIRE(level.tree.pump().has_value());
    CY_CHECK(g_log == (std::vector<std::string>{"enable:" + s, "enable:" + l}));
    g_log.clear();

    // Destroying the subtree: exit child first, then destroy child first.
    CY_REQUIRE(level.tree.destroy_node(squad).has_value());
    CY_CHECK(g_log ==
             (std::vector<std::string>{"exit:" + l, "exit:" + s, "destroy:" + l, "destroy:" + s}));
    CY_CHECK_EQ(level.bridge.attached(), 0U);
    CY_CHECK_EQ(level.runtime.live_instances(), 0U);
}

CY_TEST_CASE("a script on a node outside the tree gets no tree callback until it joins") {
    Level level;
    const scene::Node loose = level.make("Loose", scene::Node());
    CY_REQUIRE(level.bridge.attach(loose, "Probe").has_value());
    g_log.clear();
    CY_REQUIRE(level.tree.pump().has_value());
    CY_CHECK(g_log.empty());

    CY_REQUIRE(level.tree.root().add_child(loose).has_value());
    CY_REQUIRE(level.tree.pump().has_value());
    const std::string id = Level::id(loose);
    CY_CHECK(g_log == (std::vector<std::string>{"enter:" + id, "ready:" + id}));
}

CY_TEST_CASE("a script the module refuses to create leaves the node without a behaviour") {
    Level level;
    const scene::Node node = level.make("Refused", level.tree.root());
    g_refuse = true;
    const Status attached = level.bridge.attach(node, "Probe");
    CY_CHECK_FALSE(attached.has_value());
    CY_CHECK_EQ(level.bridge.attached(), 0U);
    CY_CHECK_EQ(level.tree.behaviours().instance_of(level.tree, node.entity()),
                scene::kNoBehaviourInstance);
    // And an unknown script name is NOT_FOUND rather than a crash.
    g_refuse = false;
    CY_CHECK_FALSE(level.bridge.attach(node, "Nobody").has_value());
}

CY_TEST_CASE("node_find resolves relative and absolute paths against the tree") {
    Level level;
    const scene::Node squad = level.make("Squad", level.tree.root());
    const scene::Node leader = level.make("Leader", squad);
    const scene::Node camera = level.make("Camera", squad);
    const CyEntity from = abi::to_abi(leader.entity());
    CyEntity found = CY_ENTITY_NULL;

    CY_REQUIRE_EQ(table().node_find(&level.host, from, "../Camera", &found), CY_RESULT_OK);
    CY_CHECK_EQ(found, abi::to_abi(camera.entity()));
    CY_REQUIRE_EQ(table().node_find(&level.host, from, "/Squad/Camera", &found), CY_RESULT_OK);
    CY_CHECK_EQ(found, abi::to_abi(camera.entity()));
    CY_REQUIRE_EQ(table().node_find(&level.host, abi::to_abi(squad.entity()), "Leader", &found),
                  CY_RESULT_OK);
    CY_CHECK_EQ(found, from);
    // An absolute path needs no origin.
    CY_REQUIRE_EQ(table().node_find(&level.host, CY_ENTITY_NULL, "/Squad", &found), CY_RESULT_OK);
    CY_CHECK_EQ(found, abi::to_abi(squad.entity()));

    found = 7;
    CY_CHECK_EQ(table().node_find(&level.host, from, "../Missing", &found), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().node_find(&level.host, CY_ENTITY_NULL, "Leader", &found),
                CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(found, 7U);
    abi::clear_last_error();
}
