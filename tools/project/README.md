# `tools/project/` — the project manifest and the graph built from it

Tasks 4.1, 4.2 and 4.4. `project-and-plugins` opens with the rule this directory implements:

> The **project manifest is authoritative**. Folder layout stays conventional and stops being
> load-bearing; modules declare their dependencies as public or private, cycles are rejected, and
> **architectural layering is enforced by the build**.

M0 seeded that capability on its *module* half: `cmake/modules.cmake` reads `modules/*/module.json`
and validates the graph those declare. What was missing is the thing the requirement is actually
about — a manifest for the **project**. This is it, plus the two halves of enforcement that only
exist once there is one.

| | |
|---|---|
| `schema.py` | the manifest's schema and its reader. Decides whether a manifest *parses*. |
| `graph.py` | the graph and its rejections. Decides whether what parsed is a project. |
| `project.py` | the command line: `validate`, `describe`, `emit-header`. |
| `selftest.py` | the fixtures, run and asserted. Task 4.4's evidence. |
| `fixtures/` | one project per rejection, plus one that must be accepted. |

`cmake/project.cmake` is the caller. It validates the manifest at configure time and renders
`<build>/generated/project/include/cy_project.h`, which `src/core/config/` gives names and types.

## The manifest

JSON, for the reason `module.json` is: CMake parses it with `string(JSON)` and needs no dependency,
Python parses it with the standard library, and the Rust editor will parse it at M5. One format
across the project graph means one parser to trust.

`fixtures/valid/project.json` is a complete example of every construct, and it is validated on every
run of `selftest.py`, so it cannot rot into an example that does not work.

```json
{
  "schema_version": 1,
  "project": { "name": "my-game", "version": "1.2.3", "engine_version": "0.0.0" },
  "content_roots": ["content"],
  "modules": [
    { "name": "game-core", "path": "modules/game-core",
      "layer": "core", "type": "runtime", "registration_level": "Core",
      "public_dependencies": [], "private_dependencies": [],
      "platforms": ["linux", "windows", "macos"], "hot_reload": false }
  ],
  "plugins": [
    { "id": "com.example.terrain", "version": "2.0.1", "engine_api": ">=0.0.0, <1.0.0" }
  ],
  "targets": [
    { "name": "client", "kind": "client", "shipping": true, "modules": ["game-core"] }
  ],
  "settings": { "renderer.profile": "desktop" },
  "platform_overrides": {
    "android": { "settings": { "renderer.profile": "mobile" },
                 "modules": { "game-tools": { "enabled": false } } }
  }
}
```

Every key is checked, and an unknown one is an error with a one-edit spelling suggestion where there
is one — `"hot_relaod"` is reported as `unknown key 'hot_relaod'; did you mean 'hot_reload'?`. A
manifest whose typo is ignored is a manifest that does not say what the build does.

**Layer** constrains dependencies; **registration level** orders initialisation. They are distinct
and every module carries both. `core ecs servers backends/platform scene runtime abi editor/tools`
is the layer order, mirrored from `cmake/module.cmake`; `Core Servers Scene Editor` is the level
order, mirrored from `cmake/modules.cmake`.

## The rejections

Each is a **build error**, not a warning, and each names both ends of what collided.

| Rejection | Fixture | Caught by |
|---|---|---|
| a dependency **cycle**, reported as the path that closed it | `cycle/` | `graph.py` |
| an **undeclared dependency** — a module including a header another module owns | `undeclared-dependency/` | `graph.py`, source scan |
| a dependency on a module the project does not contain | `missing-module/` | `graph.py` |
| a **layer violation** — a dependency pointing upward | `layer-violation/` | `graph.py` |
| a **private dependency that leaks** through a public header | `private-leak/` | `graph.py`, source scan |
| a **shipping target that reaches editor code**, transitively | `editor-in-shipping/` | `graph.py` |
| a mistyped or malformed manifest entry | `unknown-key/`, `malformed-json/` | `schema.py` |
| a plugin whose engine API range excludes this engine | `incompatible-plugin/` | `graph.py` |
| a layer violation through a link `cy_add_module()` never saw | `target-graph-violation/` | `cmake/project.cmake` |

The last row is task 4.2's other half and is the one that needs a real CMake run.
`cmake/module.cmake` refuses an upward link at the moment `cy_add_module()` is *given* it — that is
the check that fires in practice and it is worth keeping first, because it names the declaration
that is wrong. It is not the whole graph: a `target_link_libraries()` written after the declaration,
or a dependency inherited through an interface target, reaches a target without passing it.
`cy_project_check_target_graph()` walks what CMake will actually link, so it closes that.

### The two checks for one requirement

"Undeclared use SHALL be a build error, not a link-time accident" is checked twice, because the two
checks catch different mistakes:

* the **declared graph** catches a dependency named nowhere — a module that depends on something the
  project does not contain;
* the **source scan** catches one that is used anyway. Every header under a module's `include/`
  belongs to that module, keyed by its path relative to that include root, which is exactly the
  spelling a dependent writes in its `#include`. A module whose sources include a header owned by a
  module it did not declare is reported naming the file, both modules and the include.

The source scan is also where the private-dependency rule is enforced, since "private" is a
statement about a module's *public headers* and nothing else can see it.

## Running it

```
python3 tools/project/selftest.py                       # the fixtures — every rejection, asserted
python3 tools/project/project.py validate  --manifest project.json --engine-version 0.0.0
python3 tools/project/project.py describe  --manifest project.json
python3 tools/project/project.py emit-header --manifest project.json --output cy_project.h
```

`selftest.py` also runs as the CTest test `integration.project_graph`, declared by
`src/core/config/tests/CMakeLists.txt`, so a check that has silently stopped firing is a red build
rather than a recipe nobody remembered to run.

## `emit-header` and reproducibility

`cy_project.h` is generated rather than parsed at run time for the reason `cy_modules.h` is: one
manifest parser in the tree, in a language that has one. A second in C++ would be a second thing to
keep in agreement with the first, and the engine would pay a JSON parser in its startup path to get
it.

Output is byte-reproducible: sorted, no timestamp, no absolute path, and an unchanged file is not
rewritten, so a configure does not invalidate every translation unit that includes it.
`selftest.py` renders the same manifest into two build directories and twice into one, and requires
all three to be identical.

## The engine's own project manifest

`cmake/project.cmake` reads `CY_PROJECT_MANIFEST`, defaulting to `project.json` beside the top-level
`CMakeLists.txt`. **The engine tree does not carry one yet.** When there is none, `cy_project.h` is
rendered from the module manifests instead — still a declared graph, not one inferred from the
filesystem, because those records come from `modules/*/module.json`.

Adding `project.json` at the repository root is the remaining step, and it is a file this workstream
did not own. Copy `fixtures/valid/project.json`, replace the fixture's modules with the engine's own,
and the configure output changes from

```
-- Project graph: no project manifest; cy_project.h is rendered from the module manifests.
```

to the manifest's own summary line. Nothing else has to change: the validation, the generated header
and the runtime's module ordering already take that path, and it is exercised today with
`-DCY_PROJECT_MANIFEST=tools/project/fixtures/valid/project.json`.
