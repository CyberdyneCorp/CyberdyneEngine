// The project graph, as the runtime sees it. Task 4.1.
//
// The graph's *rejections* — cycles, undeclared dependencies, upward layer dependencies, a shipping
// target that reaches editor code — are configure-time failures and are proved by
// tools/project/selftest.py against the fixtures under tools/project/fixtures/. They cannot be
// proved from inside a program that only compiles because the graph was accepted.
//
// What is provable here is the other half: that what the graph accepted reaches the engine intact,
// in registration order, and that the runtime's module registry takes its order from it.

#include <cy/core/config/module_registry.h>
#include <cy/core/config/project.h>
#include <cy/test/test.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

using cy::config::ModuleLevel;
using cy::config::ProjectModule;

}  // namespace

CY_TEST_CASE("Structure is declared: the project graph names itself and its modules") {
    const cy::config::ProjectInfo& info = cy::config::project();

    CY_CHECK(info.name != nullptr);
    CY_CHECK(std::strlen(info.name) > 0);
    CY_CHECK(info.version != nullptr);
    CY_CHECK(info.engine_version != nullptr);
    CY_CHECK(info.platform != nullptr);

    // Whether this tree carries a project manifest of its own is a build-configuration question,
    // and both answers are legitimate — so the case asserts the *consequence* rather than the
    // answer: either way the modules came from a manifest, never from a directory listing.
    CY_TEST_MESSAGE("project '" << info.name << "' " << info.version << ", " << info.modules.size()
                                << " module(s), manifest "
                                << (info.manifest_present ? "present" : "absent"));
    for (const ProjectModule& module : info.modules) {
        CY_CHECK(module.name != nullptr);
        CY_CHECK(std::strlen(module.name) > 0);
        CY_CHECK(std::strlen(module.layer) > 0);
        CY_CHECK(std::strlen(module.level_name) > 0);
        CY_CHECK(std::strcmp(module.level_name, cy::config::module_level_name(module.level)) == 0);
        CY_CHECK(module.layer_index >= 0);
        CY_CHECK(module.layer_index <= 7);
    }
}

CY_TEST_CASE("the module table is in registration order") {
    // Level, then name — the order the runtime initialises in. Asserted over whatever the build's
    // graph happens to contain, so it keeps holding as modules are added.
    const ProjectModule* previous = nullptr;
    for (const ProjectModule& module : cy::config::project().modules) {
        if (previous != nullptr) {
            const bool ordered =
                static_cast<int>(previous->level) < static_cast<int>(module.level) ||
                (previous->level == module.level && std::strcmp(previous->name, module.name) <= 0);
            CY_CHECK(ordered);
        }
        previous = &module;
    }
}

CY_TEST_CASE("a module is found by name, and nothing is found by a name nothing has") {
    for (const ProjectModule& module : cy::config::project().modules) {
        const ProjectModule* found = cy::config::find_project_module(module.name);
        CY_REQUIRE(found != nullptr);
        CY_CHECK_EQ(found->level, module.level);
    }
    CY_CHECK(cy::config::find_project_module("no-such-module") == nullptr);
    CY_CHECK(cy::config::find_project_module(nullptr) == nullptr);
    CY_CHECK(cy::config::find_project_target("no-such-target") == nullptr);
    CY_CHECK(cy::config::find_project_plugin("no.such.plugin") == nullptr);
}

CY_TEST_CASE("Renaming a plugin is safe: a plugin resolves by identifier") {
    for (const cy::config::ProjectPlugin& plugin : cy::config::project().plugins) {
        CY_CHECK(std::strlen(plugin.id) > 0);
        CY_CHECK(cy::config::find_project_plugin(plugin.id) == &plugin);
    }
}

CY_TEST_CASE("the module registry takes its order from the project graph") {
    cy::config::ModuleRegistry registry;
    CY_REQUIRE(registry.add_project_modules());
    CY_CHECK_EQ(registry.size(), cy::config::project().modules.size());

    std::vector<std::string> from_graph;
    for (const ProjectModule& module : cy::config::project().modules) {
        from_graph.emplace_back(module.name);
    }
    std::vector<std::string> from_registry;
    for (const cy::config::ModuleRegistration& registration : registry.modules()) {
        from_registry.emplace_back(registration.name);
    }
    CY_CHECK(from_graph == from_registry);

    // Every level starts and stops cleanly over whatever the graph declared, including none.
    for (const ModuleLevel level :
         {ModuleLevel::Core, ModuleLevel::Servers, ModuleLevel::Scene, ModuleLevel::Editor}) {
        CY_REQUIRE(registry.start(level));
    }
    for (const ModuleLevel level :
         {ModuleLevel::Editor, ModuleLevel::Scene, ModuleLevel::Servers, ModuleLevel::Core}) {
        registry.stop(level);
    }
    CY_CHECK(registry.journal_is_reversed());
}

CY_TEST_CASE("build targets carry a kind and a shipping flag") {
    // "Shipping excludes editor code" is a property of the *whole reachable set*, which only the
    // configure-time graph has — tools/project/graph.py refuses a shipping target that reaches an
    // editor module, and the `editor-in-shipping` fixture proves the refusal still fires. The
    // generated table carries the outcome, so this asserts what reached the engine.
    for (const cy::config::ProjectTarget& target : cy::config::project().targets) {
        CY_CHECK(std::strlen(target.name) > 0);
        const bool known =
            std::strcmp(target.kind, "client") == 0 || std::strcmp(target.kind, "server") == 0 ||
            std::strcmp(target.kind, "editor") == 0 || std::strcmp(target.kind, "tool") == 0;
        CY_CHECK(known);
        CY_CHECK(cy::config::find_project_target(target.name) == &target);
        if (target.shipping) {
            CY_CHECK(std::strcmp(target.kind, "editor") != 0);
        }
    }
}
