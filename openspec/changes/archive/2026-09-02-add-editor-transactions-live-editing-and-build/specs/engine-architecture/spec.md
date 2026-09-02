## MODIFIED Requirements

### Requirement: Module system
Optional functionality SHALL be packaged as **modules** under `modules/<name>/`, each providing
a `CMakeLists.txt`, a manifest declaring name, description, **layer**, **type**, public and private
dependencies, default-enabled flag, supported platforms, and hot-reload support, and registration
entry points.

Module manifests and their dependency graph SHALL be governed by `project-and-plugins`: the project
graph is authoritative, undeclared dependencies and cycles are build errors, and **architectural
layering is enforced** — a runtime module SHALL NOT depend on an editor module, and a foundation
module SHALL NOT depend upward.

Modules SHALL register at one of four levels: `Core`, `Servers`, `Scene`, `Editor`. Registration
level orders initialisation; **layer** constrains dependencies. The two are distinct.

#### Scenario: Module registers a backend
- **WHEN** the `jolt-physics` module initialises at the `Servers` level
- **THEN** it SHALL register its factory with `PhysicsServer` before the runtime constructs the
  physics server

#### Scenario: Module disabled at configure time
- **WHEN** CMake is configured with `-DCY_MODULE_NAVIGATION=OFF`
- **THEN** the module's sources SHALL be excluded and `CY_MODULE_NAVIGATION` SHALL be undefined
  in the generated `cy_modules.h`

#### Scenario: Third-party module
- **WHEN** a path is passed via `CY_EXTRA_MODULE_PATHS`
- **THEN** modules found there SHALL build exactly as in-tree modules do

#### Scenario: Layering is enforced, not advised
- **WHEN** a module declares a dependency that violates the layer ordering
- **THEN** the build SHALL fail naming both modules and their layers
