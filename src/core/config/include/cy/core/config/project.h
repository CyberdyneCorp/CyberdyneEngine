// The project graph, as the runtime sees it.
//
// Task 4.1. `project-and-plugins`: "A project SHALL be described by a declarative project manifest
// naming: the project and its version, the engine version it targets, its modules, its plugins and
// their versions, its content roots, its build targets, and its per-platform overrides. The
// manifest SHALL be the authority on structure." This is that manifest, after validation, in a form
// the engine can read with no file I/O and no allocation.
//
// IT IS GENERATED, NOT PARSED HERE. cmake/project.cmake runs tools/project/project.py at configure
// time: the manifest is validated — cycles, undeclared dependencies, upward layer dependencies and
// a shipping target that reaches editor code are configure errors — and what survives is rendered
// into <cy_project.h>. There is one manifest parser in this tree and it is in the language that has
// one. A second in C++ would be a second thing to keep in agreement with the first.
//
// WHEN THERE IS NO PROJECT MANIFEST, `manifest_present` is false and the modules are the ones
// modules/*/module.json declared. That is still a declared graph and not an inferred one; it is the
// engine building itself rather than a game.
//
// Every pointer here is a string literal in the generated translation unit and outlives the
// process.

#pragma once

#include <cy/core/base/types.h>
#include <cy/core/config/module_registry.h>

#include <span>

namespace cy::config {

/// One module of the project graph. `layer` constrains what it may depend on; `level` decides when
/// it initialises. The two are distinct and every module carries both.
struct ProjectModule {
    const char* name = "";
    const char* layer = "";
    i32 layer_index = 0;
    const char* level_name = "";
    ModuleLevel level = ModuleLevel::Core;
    const char* type = "";
    bool hot_reload = false;
};

/// A plugin as the manifest declares it. Identity is `id` and never the display name or the path,
/// so a plugin can be renamed or moved without invalidating a project that uses it.
struct ProjectPlugin {
    const char* id = "";
    const char* version = "";
    const char* engine_api = "";
    bool enabled = false;
};

struct ProjectTarget {
    const char* name = "";
    const char* kind = "";
    /// A shipping target is the one the graph excludes editor code from. Enforced at configure time
    /// by tools/project/graph.py, which refuses a shipping target that reaches an editor module.
    bool shipping = false;
};

/// A setting the manifest supplied, with the configuration layer it belongs to. The text is parsed
/// against the setting's declared type by ConfigStore::load_project_settings() — the manifest
/// carries values, the engine's schema carries types.
struct ProjectSetting {
    const char* layer = "";
    const char* key = "";
    const char* value = "";
};

struct ProjectInfo {
    const char* name = "";
    const char* version = "";
    const char* engine_version = "";
    /// The platform the graph was resolved for, so a reader can tell which per-platform overrides
    /// were applied. Empty when the build did not name one.
    const char* platform = "";
    /// False when the tree carries no project manifest and the modules came from module manifests.
    bool manifest_present = false;

    std::span<const ProjectModule> modules;
    std::span<const char* const> content_roots;
    std::span<const ProjectPlugin> plugins;
    std::span<const ProjectTarget> targets;
    std::span<const ProjectSetting> settings;
};

/// This build's project graph. The same object every call; there is exactly one project.
const ProjectInfo& project();

/// Lookup by name. Null when the graph does not contain it, which is the answer a caller acts on —
/// "not in the manifest" and "not in the project" are the same statement.
const ProjectModule* find_project_module(const char* name);
const ProjectTarget* find_project_target(const char* name);
const ProjectPlugin* find_project_plugin(const char* id);

}  // namespace cy::config
