#include <cy/core/config/project.h>

#include <cy_project.h>

#include <cstring>

// The tables in <cy_project.h> are X-macros: one line per entry, expanded here into the arrays the
// interface hands out. Rendering them as macros rather than as a struct literal is what lets a
// consumer that wants a different shape — a diagnostic, a report, the editor at M5 — expand the
// same table for itself without this file having to anticipate it.

namespace cy::config {
namespace {

// Trailing commas are legal in an aggregate initialiser, which is what makes a table of N entries
// and a table of one expand identically.
#define CY_PROJECT_MODULE_ENTRY(name, layer, layer_index, level, level_index, type, hot_reload)  \
    ProjectModule{                                                                               \
        (name), (layer),          (layer_index), (level), static_cast<ModuleLevel>(level_index), \
        (type), (hot_reload) != 0},

#define CY_PROJECT_PLUGIN_ENTRY(id, version, engine_api, enabled) \
    ProjectPlugin{(id), (version), (engine_api), (enabled) != 0},

#define CY_PROJECT_TARGET_ENTRY(name, kind, shipping) \
    ProjectTarget{(name), (kind), (shipping) != 0},

#define CY_PROJECT_SETTING_ENTRY(layer, key, value) ProjectSetting{(layer), (key), (value)},

#define CY_PROJECT_CONTENT_ROOT_ENTRY(path) (path),

#if CY_PROJECT_MODULE_COUNT > 0
const ProjectModule kModules[] = {CY_PROJECT_MODULE_TABLE(CY_PROJECT_MODULE_ENTRY)};
#endif
#if CY_PROJECT_CONTENT_ROOT_COUNT > 0
const char* const kContentRoots[] = {CY_PROJECT_CONTENT_ROOT_TABLE(CY_PROJECT_CONTENT_ROOT_ENTRY)};
#endif
#if CY_PROJECT_PLUGIN_COUNT > 0
const ProjectPlugin kPlugins[] = {CY_PROJECT_PLUGIN_TABLE(CY_PROJECT_PLUGIN_ENTRY)};
#endif
#if CY_PROJECT_TARGET_COUNT > 0
const ProjectTarget kTargets[] = {CY_PROJECT_TARGET_TABLE(CY_PROJECT_TARGET_ENTRY)};
#endif
#if CY_PROJECT_SETTING_COUNT > 0
const ProjectSetting kSettings[] = {CY_PROJECT_SETTING_TABLE(CY_PROJECT_SETTING_ENTRY)};
#endif

// A zero-length array is not a thing C++ has, so an empty table is an empty span over nothing
// rather than a span over an array of no elements.
template <typename T, usize N>
std::span<const T> table(const T (&entries)[N]) {
    return {entries, N};
}

ProjectInfo build() {
    ProjectInfo info;
    info.name = CY_PROJECT_NAME;
    info.version = CY_PROJECT_VERSION;
    info.engine_version = CY_PROJECT_ENGINE_VERSION;
    info.platform = CY_PROJECT_PLATFORM;
    info.manifest_present = CY_PROJECT_MANIFEST_PRESENT != 0;
#if CY_PROJECT_MODULE_COUNT > 0
    info.modules = table(kModules);
#endif
#if CY_PROJECT_CONTENT_ROOT_COUNT > 0
    info.content_roots = table(kContentRoots);
#endif
#if CY_PROJECT_PLUGIN_COUNT > 0
    info.plugins = table(kPlugins);
#endif
#if CY_PROJECT_TARGET_COUNT > 0
    info.targets = table(kTargets);
#endif
#if CY_PROJECT_SETTING_COUNT > 0
    info.settings = table(kSettings);
#endif
    return info;
}

bool same(const char* left, const char* right) {
    return left != nullptr && right != nullptr && std::strcmp(left, right) == 0;
}

}  // namespace

const ProjectInfo& project() {
    // A function-local static: constructed on first use, so the graph is readable from anywhere
    // without depending on the order translation units initialise in — which is the same reason
    // module registration is an explicit call rather than a static initialiser.
    static const ProjectInfo info = build();
    return info;
}

const ProjectModule* find_project_module(const char* name) {
    for (const ProjectModule& module : project().modules) {
        if (same(module.name, name)) {
            return &module;
        }
    }
    return nullptr;
}

const ProjectTarget* find_project_target(const char* name) {
    for (const ProjectTarget& target : project().targets) {
        if (same(target.name, name)) {
            return &target;
        }
    }
    return nullptr;
}

const ProjectPlugin* find_project_plugin(const char* id) {
    for (const ProjectPlugin& plugin : project().plugins) {
        if (same(plugin.id, id)) {
            return &plugin;
        }
    }
    return nullptr;
}

}  // namespace cy::config
