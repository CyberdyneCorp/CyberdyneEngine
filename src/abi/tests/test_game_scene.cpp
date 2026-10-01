// SPDX-License-Identifier: MIT
// ABI 1.5's `node_find` against a fake backend, and the five tree callbacks through
// `BehaviourRuntime::tree_callback`. `add-swift-m12-gaps`.
//
// The real tree — the pump's order, a path resolved against a scene — is the adapter's, and
// integration.game_backend_scene tests it over `cy::scene::SceneTree`.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/scene.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/abi/module.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstddef>
#include <cstring>

namespace {

using cy::u32;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Scripting);
}

class FakeScene final : public cy::abi::game::SceneBackend {
public:
    mutable CyEntity last_from = CY_ENTITY_NULL;
    mutable const char* last_path = "";

    CyResult find(CyEntity from, const char* path, CyEntity& out) const noexcept override {
        last_from = from;
        last_path = path;
        if (std::strcmp(path, "../Camera") == 0) {
            out = 42;
            return CY_RESULT_OK;
        }
        return cy::abi::report(CY_RESULT_NOT_FOUND, "no node at that path");
    }
};

// A behaviour vtable with every tree callback, recording the order it was called in.
char g_calls[16] = {};
u32 g_call_count = 0;

void record(char what) noexcept {
    if (g_call_count < sizeof(g_calls) - 1U) {
        g_calls[g_call_count++] = what;
    }
}

extern "C" CyInstance tree_probe_create(CyEngine, CyEntity, void*) {
    static int instance = 0;
    return &instance;
}
extern "C" void tree_probe_destroy(CyInstance, void*) {}
extern "C" void tree_probe_enter(CyInstance, void*) {
    record('E');
}
extern "C" void tree_probe_ready(CyInstance, void*) {
    record('R');
}
extern "C" void tree_probe_enable(CyInstance, void*) {
    record('+');
}
extern "C" void tree_probe_disable(CyInstance, void*) {
    record('-');
}
extern "C" void tree_probe_exit(CyInstance, void*) {
    record('X');
}

CyBehaviourVTable tree_vtable() noexcept {
    CyBehaviourVTable vtable{};
    vtable.struct_size = sizeof(vtable);
    vtable.create = &tree_probe_create;
    vtable.destroy = &tree_probe_destroy;
    vtable.enter_tree = &tree_probe_enter;
    vtable.ready = &tree_probe_ready;
    vtable.enable = &tree_probe_enable;
    vtable.disable = &tree_probe_disable;
    vtable.exit_tree = &tree_probe_exit;
    return vtable;
}

}  // namespace

CY_TEST_CASE("node_find resolves through the scene backend in every phase") {
    cy::abi::Host host(allocator());
    const CyInterface& iface = table();
    CyEntity found = CY_ENTITY_NULL;
    CY_CHECK_EQ(iface.node_find(&host, 7, "../Camera", &found), CY_RESULT_UNAVAILABLE);

    FakeScene scene;
    host.game.scene = &scene;
    for (const CyPhase phase : {CY_PHASE_NONE, CY_PHASE_FIXED_UPDATE, CY_PHASE_FRAME_UPDATE}) {
        const cy::abi::game::PhaseScope scope(host.game.clock, phase);
        found = CY_ENTITY_NULL;
        CY_CHECK_EQ(iface.node_find(&host, 7, "../Camera", &found), CY_RESULT_OK);
        CY_CHECK_EQ(found, 42U);
    }
    CY_CHECK_EQ(scene.last_from, 7U);

    // A path that does not resolve is NOT_FOUND and leaves the output alone — which is what makes
    // `@Node` nil rather than a trap.
    found = 99;
    CY_CHECK_EQ(iface.node_find(&host, 7, "Missing", &found), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(found, 99U);
    CY_CHECK_EQ(iface.node_find(&host, 7, "", &found), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.node_find(&host, 7, nullptr, &found), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.node_find(&host, 7, "../Camera", nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.node_find(nullptr, 7, "../Camera", &found), CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("the tree callbacks reach the instance through its own generation's vtable") {
    cy::abi::Host host(allocator());
    cy::abi::BehaviourRuntime runtime(allocator(), host);
    g_call_count = 0;
    std::memset(g_calls, 0, sizeof(g_calls));
    CY_REQUIRE(host.register_behaviour("Probe", tree_vtable()).has_value());
    const cy::Expected<u32, cy::Error> slot = runtime.create("Probe", 5);
    CY_REQUIRE(slot.has_value());

    using cy::abi::TreeCallback;
    for (const TreeCallback callback :
         {TreeCallback::EnterTree, TreeCallback::Ready, TreeCallback::Disable, TreeCallback::Enable,
          TreeCallback::ExitTree}) {
        CY_CHECK(runtime.tree_callback(slot.value(), callback));
    }
    CY_CHECK(std::strcmp(g_calls, "ER-+X") == 0);
    // An empty slot is not dispatched.
    CY_CHECK_FALSE(runtime.tree_callback(slot.value() + 1U, TreeCallback::Ready));
}

CY_TEST_CASE(
    "a module compiled before 1.5 gets no tree callback, and is not read past its vtable") {
    cy::abi::Host host(allocator());
    cy::abi::BehaviourRuntime runtime(allocator(), host);
    g_call_count = 0;
    std::memset(g_calls, 0, sizeof(g_calls));
    // A 1.4 vtable ends at `frame_update`. The bytes after it are garbage the engine must not read.
    CyBehaviourVTable old = tree_vtable();
    old.struct_size = static_cast<uint32_t>(offsetof(CyBehaviourVTable, enter_tree));
    CY_REQUIRE(host.register_behaviour("Old", old).has_value());
    const cy::Expected<u32, cy::Error> slot = runtime.create("Old", 5);
    CY_REQUIRE(slot.has_value());
    CY_CHECK_FALSE(runtime.tree_callback(slot.value(), cy::abi::TreeCallback::EnterTree));
    CY_CHECK_FALSE(runtime.tree_callback(slot.value(), cy::abi::TreeCallback::Ready));
    CY_CHECK_EQ(g_call_count, 0U);
}
