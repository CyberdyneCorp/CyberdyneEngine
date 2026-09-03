# `modules/` — optional functionality

Each subdirectory is one module: a `module.json` manifest, a `CMakeLists.txt` declaring exactly one
target through `cy_add_module()`, a public header directory, and its sources.
`modules/example-null/` is the template.

Nothing lists the modules. `cmake/modules.cmake` discovers them from their manifests, declares a
`CY_MODULE_<NAME>` option for each, validates the graph they form, and adds the enabled ones in
registration order.

## The manifest

`module.json`, read by CMake's own `string(JSON)` — no parser, no dependency, and the same file is
readable by the Python generators and, at M5, by the Rust editor. Every key is required, and an
unknown key is a configure error: a manifest whose typo is ignored is a manifest that does not say
what the build does.

| Key | Type | Meaning |
|---|---|---|
| `name` | string | The module's identity in the graph. Must equal the directory name. |
| `description` | string | One line. It becomes the `CY_MODULE_<NAME>` option's help text. |
| `layer` | string | `core`, `ecs`, `servers`, `backends`, `platform`, `scene`, `runtime`, `abi`, `editor`, `tools`. **Constrains dependencies.** |
| `type` | string | `runtime`, `editor`, `developer`, `server`, `tool`, `third-party`. |
| `registration_level` | string | `Core`, `Servers`, `Scene`, `Editor`. **Orders initialisation.** |
| `public_dependencies` | array | Modules visible through this module's own interface. |
| `private_dependencies` | array | Modules used in the implementation, never exposed to dependents. |
| `default_enabled` | boolean | The `CY_MODULE_<NAME>` default on a platform the module supports. |
| `platforms` | array | `linux`, `windows`, `macos`, `ios`, `android`, `visionos`, `web`. |
| `hot_reload` | boolean | Whether the module can be reloaded without restarting. |

**Layer and registration level are distinct.** Layer says what a module may depend on; registration
level says when it initialises. A `Scene`-level module may sit at the core layer, and a core-layer
module may register at `Servers`.

## Names are derived, not chosen

For a module called `example-null`:

| | |
|---|---|
| Option | `CY_MODULE_EXAMPLE_NULL` |
| Target | `cy_module_example_null` |
| Generated macro | `CY_MODULE_EXAMPLE_NULL`, defined to 1 when enabled and undefined when not |

A module that declares a differently named target fails the configure, because the graph checked
against the manifests would otherwise not be the graph that was built.

## What the build enforces

Every one of these is a configure error naming what collided, per `project-and-plugins`:

- a dependency on a module that does not exist;
- a dependency on a module that is disabled while the dependent is enabled;
- a dependency that points **upward** through the layers, naming both modules and both layers;
- a **cycle**, reported as the path that closed it;
- a target that links another module the manifest does not declare — the manifest is authoritative;
- a private dependency exposed through the target's interface;
- an unknown, missing or mistyped manifest key;
- two modules with the same name;
- an enabled module whose `platforms` do not include the platform being built.

## Out-of-tree modules

`-DCY_EXTRA_MODULE_PATHS=/path/to/module` — or to a directory of modules — discovers them exactly as
in-tree ones: same manifest, same option, same graph validation, same registration order.

**Governed by**: `project-and-plugins`, `engine-architecture` (module system).
