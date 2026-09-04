// Lifecycle callbacks and their order. Task 3.1.6.
//
// The callbacks are function pointers, so the record of what ran is a file-scope structure rather
// than a captured lambda. That is the shape a native behaviour has anyway: the instance's own state
// is `BehaviourContext::state`, and everything else it reaches is a component.

#include <cy/test/test.h>

#include <cy/scene/behaviour.h>
#include <cy/scene/tree.h>

#include "fixtures.h"

#include <cstring>

using cy::scene::test::Fixture;
using cy::scene::test::make_child;

namespace {

/// What ran, in order, as `<callback>:<node name>`.
struct Log {
    static constexpr cy::u32 kMax = 64;
    const char* callback[kMax] = {};
    cy::u32 name[kMax] = {};
    cy::u32 count = 0;

    void record(const char* what, cy::Name node) noexcept {
        if (count < kMax) {
            callback[count] = what;
            name[count] = node.index();
            ++count;
        }
    }
    void clear() noexcept { count = 0; }
    [[nodiscard]] bool at(cy::u32 index, const char* what, cy::Name node) const noexcept {
        return index < count && std::strcmp(callback[index], what) == 0 &&
               name[index] == node.index();
    }
};

Log g_log;

void on_create(const cy::scene::BehaviourContext& context) noexcept {
    g_log.record("onCreate", context.node.name());
}
void on_enter_tree(const cy::scene::BehaviourContext& context) noexcept {
    g_log.record("onEnterTree", context.node.name());
}
void on_ready(const cy::scene::BehaviourContext& context) noexcept {
    g_log.record("onReady", context.node.name());
}
void on_exit_tree(const cy::scene::BehaviourContext& context) noexcept {
    g_log.record("onExitTree", context.node.name());
}
void on_destroy(const cy::scene::BehaviourContext& context) noexcept {
    g_log.record("onDestroy", context.node.name());
}
void on_enable(const cy::scene::BehaviourContext& context) noexcept {
    g_log.record("onEnable", context.node.name());
}
void on_disable(const cy::scene::BehaviourContext& context) noexcept {
    g_log.record("onDisable", context.node.name());
}

[[nodiscard]] cy::scene::BehaviourDesc tracer() noexcept {
    cy::scene::BehaviourDesc desc;
    desc.name = "Tracer";
    desc.on_create = &on_create;
    desc.on_enter_tree = &on_enter_tree;
    desc.on_ready = &on_ready;
    desc.on_exit_tree = &on_exit_tree;
    desc.on_destroy = &on_destroy;
    desc.on_enable = &on_enable;
    desc.on_disable = &on_disable;
    return desc;
}

}  // namespace

CY_TEST_CASE("onReady runs child-first, exactly once per attachment") {
    g_log.clear();
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const auto type = fixture.tree.behaviours().add(fixture.world, tracer());
    CY_REQUIRE(type.has_value());

    // Built detached, so the whole subtree attaches in one edge and the order is the interesting
    // thing rather than an artefact of the order the nodes were created in.
    cy::scene::Node parent =
        *fixture.tree.create_node(cy::Name::intern("Parent"), cy::scene::Node());
    const cy::scene::Node child = make_child(fixture.tree, parent, "Child");
    const cy::scene::Node grandchild = make_child(fixture.tree, child, "Grandchild");
    for (const cy::scene::Node node : {parent, child, grandchild}) {
        CY_REQUIRE(fixture.tree.behaviours().attach(fixture.tree, node, *type).has_value());
    }
    // Three `onCreate`s and nothing else: nothing is in the tree yet.
    CY_CHECK_EQ(g_log.count, 3U);
    CY_CHECK(g_log.at(0, "onCreate", parent.name()));

    g_log.clear();
    CY_REQUIRE(parent.set_parent(fixture.tree.root()).has_value());
    // Queued, not fired: a subtree is attached one edge at a time and `onReady` is defined as
    // running after all children are ready.
    CY_CHECK_EQ(g_log.count, 0U);

    CY_REQUIRE(fixture.tree.pump().has_value());
    CY_REQUIRE_EQ(g_log.count, 6U);
    // Parent-first for onEnterTree...
    CY_CHECK(g_log.at(0, "onEnterTree", parent.name()));
    CY_CHECK(g_log.at(1, "onEnterTree", child.name()));
    CY_CHECK(g_log.at(2, "onEnterTree", grandchild.name()));
    // ...and child-first for onReady: "every descendant SHALL receive onReady before its
    // ancestors".
    CY_CHECK(g_log.at(3, "onReady", grandchild.name()));
    CY_CHECK(g_log.at(4, "onReady", child.name()));
    CY_CHECK(g_log.at(5, "onReady", parent.name()));

    // Exactly once: a second pump with nothing queued fires nothing.
    g_log.clear();
    CY_REQUIRE(fixture.tree.pump().has_value());
    CY_CHECK_EQ(g_log.count, 0U);
}

CY_TEST_CASE("leaving and re-entering the tree fires exit and ready again") {
    g_log.clear();
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const auto type = fixture.tree.behaviours().add(fixture.world, tracer());
    CY_REQUIRE(type.has_value());
    cy::scene::Node parent = make_child(fixture.tree, fixture.tree.root(), "Parent");
    const cy::scene::Node child = make_child(fixture.tree, parent, "Child");
    CY_REQUIRE(fixture.tree.behaviours().attach(fixture.tree, parent, *type).has_value());
    CY_REQUIRE(fixture.tree.behaviours().attach(fixture.tree, child, *type).has_value());
    CY_REQUIRE(fixture.tree.pump().has_value());

    g_log.clear();
    CY_REQUIRE(parent.set_parent(cy::scene::Node()).has_value());
    CY_REQUIRE(fixture.tree.pump().has_value());
    // Child-first, as the table says.
    CY_REQUIRE_EQ(g_log.count, 2U);
    CY_CHECK(g_log.at(0, "onExitTree", child.name()));
    CY_CHECK(g_log.at(1, "onExitTree", parent.name()));

    g_log.clear();
    CY_REQUIRE(parent.set_parent(fixture.tree.root()).has_value());
    CY_REQUIRE(fixture.tree.pump().has_value());
    // "once per attachment unless re-requested" — this is a new attachment, so onReady runs again.
    CY_CHECK_EQ(g_log.count, 4U);
    CY_CHECK(g_log.at(2, "onReady", child.name()));
    CY_CHECK(g_log.at(3, "onReady", parent.name()));
}

CY_TEST_CASE("onDestroy runs child-first while the components still exist") {
    g_log.clear();
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const auto type = fixture.tree.behaviours().add(fixture.world, tracer());
    CY_REQUIRE(type.has_value());
    cy::scene::Node parent = make_child(fixture.tree, fixture.tree.root(), "Parent");
    const cy::scene::Node child = make_child(fixture.tree, parent, "Child");
    CY_REQUIRE(fixture.tree.behaviours().attach(fixture.tree, parent, *type).has_value());
    CY_REQUIRE(fixture.tree.behaviours().attach(fixture.tree, child, *type).has_value());
    CY_REQUIRE(fixture.tree.pump().has_value());

    g_log.clear();
    CY_REQUIRE(parent.destroy().has_value());
    CY_REQUIRE_EQ(g_log.count, 4U);
    CY_CHECK(g_log.at(0, "onExitTree", cy::Name::intern("Child")));
    CY_CHECK(g_log.at(1, "onExitTree", cy::Name::intern("Parent")));
    CY_CHECK(g_log.at(2, "onDestroy", cy::Name::intern("Child")));
    CY_CHECK(g_log.at(3, "onDestroy", cy::Name::intern("Parent")));
    CY_CHECK_EQ(fixture.tree.stats().behaviour_instances, 0U);
}

CY_TEST_CASE("onEnable and onDisable follow the effective state, published by propagation") {
    g_log.clear();
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const auto type = fixture.tree.behaviours().add(fixture.world, tracer());
    CY_REQUIRE(type.has_value());
    cy::scene::Node parent = make_child(fixture.tree, fixture.tree.root(), "Parent");
    const cy::scene::Node child = make_child(fixture.tree, parent, "Child");
    CY_REQUIRE(fixture.tree.behaviours().attach(fixture.tree, child, *type).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_REQUIRE(fixture.tree.pump().has_value());

    g_log.clear();
    CY_REQUIRE(parent.set_enabled(false).has_value());
    // Nothing yet: effective enablement is published by propagation, not by the write.
    CY_REQUIRE(fixture.tree.pump().has_value());
    CY_CHECK_EQ(g_log.count, 0U);

    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_REQUIRE(fixture.tree.pump().has_value());
    CY_REQUIRE_EQ(g_log.count, 1U);
    CY_CHECK(g_log.at(0, "onDisable", cy::Name::intern("Child")));

    g_log.clear();
    CY_REQUIRE(parent.set_enabled(true).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_REQUIRE(fixture.tree.pump().has_value());
    CY_REQUIRE_EQ(g_log.count, 1U);
    CY_CHECK(g_log.at(0, "onEnable", cy::Name::intern("Child")));
}

CY_TEST_CASE("an unimplemented callback is in no dispatch list and costs nothing") {
    g_log.clear();
    Fixture fixture;
    CY_REQUIRE(fixture.start());

    // A behaviour that implements only `onCreate`. Everything else is a null pointer, which is the
    // whole of "callbacks are opt-in".
    cy::scene::BehaviourDesc quiet;
    quiet.name = "Quiet";
    quiet.on_create = &on_create;
    const auto type = fixture.tree.behaviours().add(fixture.world, quiet);
    CY_REQUIRE(type.has_value());

    cy::scene::Node node = make_child(fixture.tree, fixture.tree.root(), "Quiet");
    CY_REQUIRE(fixture.tree.behaviours().attach(fixture.tree, node, *type).has_value());
    CY_CHECK_EQ(g_log.count, 1U);
    CY_REQUIRE(fixture.tree.pump().has_value());
    CY_CHECK_EQ(g_log.count, 1U);

    cy::Array<cy::scene::BehaviourReport> report(cy::scene::test::allocator());
    CY_REQUIRE(fixture.tree.behaviours().report(report).has_value());
    CY_REQUIRE_EQ(report.size(), 1U);
    CY_CHECK_EQ(report[0].callbacks, 1U);  // onCreate only
    CY_CHECK(report[0].dispatch == cy::scene::BehaviourDispatch::None);
}
