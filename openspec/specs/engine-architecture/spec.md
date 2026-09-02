# engine-architecture Specification

## Purpose

Defines the top-level architecture of **CyberdyneEngine**: an open-source game engine written in
C++20 with Swift as the gameplay scripting language. It establishes the layer stack, the
server pattern that decouples simulation from subsystems, the ECS-core / scene-graph-façade
duality, the module system, deterministic startup and shutdown, and the main loop.

Influences are drawn deliberately: the **server + handle** architecture and scene-tree
ergonomics from Godot, **component composition** and asset/prefab workflow from Unity, and the
**render graph**, gameplay framework, and tooling ambition from Unreal. CyberdyneEngine is not a
port of any of them.

## Requirements

### Requirement: Layered architecture
The engine SHALL be organised into strictly layered modules where a lower layer never depends on
a higher one, enforced at build time by target-level dependency declarations.

| Layer | Directory | Responsibility |
|---|---|---|
| Core | `src/core/` | Type system, memory, containers, math, jobs, assets, platform abstraction |
| ECS | `src/ecs/` | Archetype world, components, queries, system scheduling |
| Servers | `src/servers/` | Handle-based services: render, physics, audio, navigation, text, display |
| Backends | `src/backends/` | Concrete implementations: Vulkan, Metal, Jolt, platform audio/input |
| Scene | `src/scene/` | Node façade, transforms, prefabs, built-in components and node types |
| Runtime | `src/runtime/` | Engine bootstrap, main loop, subsystem wiring |
| ABI | `src/abi/` | The stable flat C ABI exported to scripting and extensions |
| Bindings | `bindings/swift/` | Generated Swift overlay and the `CyberdyneKit` package |
| Editor | `editor/` | Editor application, built on Scene + Runtime, tools builds only |
| Tools | `tools/` | Code generators, asset cookers, CLI utilities |

#### Scenario: Core cannot depend on Scene
- **WHEN** a translation unit under `src/core/` is compiled
- **THEN** it SHALL NOT include headers from `src/scene/`, `src/servers/`, or `editor/`
- **AND** the build SHALL fail if such a dependency is introduced

#### Scenario: Editor is absent from shipping builds
- **WHEN** the engine is configured with `CY_BUILD_EDITOR=OFF`
- **THEN** `editor/` SHALL be excluded and `CY_EDITOR` SHALL be undefined
- **AND** code guarded by `#if CY_EDITOR` SHALL be removed from the binary

### Requirement: C++20 as the implementation language
Engine code SHALL target **C++20** and SHALL make use of concepts for template constraints,
`std::span` for non-owning views, designated initializers for descriptor structs, `constexpr`
evaluation for compile-time tables, three-way comparison where ordering is needed, and
coroutines only where a subsystem explicitly documents their use.

The engine SHALL be compiled with **exceptions and RTTI disabled** (`-fno-exceptions`,
`-fno-rtti`); fallible operations SHALL return `cy::Expected<T, Error>` and programmer errors
SHALL use the assertion macros in `core/diagnostics`.

C++20 **modules** SHALL NOT be required: headers are the canonical interface form. The build
MAY offer an experimental modules path, and migrating to modules is an explicitly deferred
decision.

#### Scenario: Fallible call
- **WHEN** an operation can fail for reasons the caller must handle (file missing, device lost)
- **THEN** it SHALL return `cy::Expected<T, Error>` rather than throwing or returning a bare
  error code

#### Scenario: Programmer error
- **WHEN** an invariant that no correct caller can violate is broken
- **THEN** `CY_ASSERT` SHALL fire in debug and development builds, and the condition SHALL be
  documented as a precondition rather than checked in shipping builds

### Requirement: Server architecture
Heavy subsystems SHALL be exposed as **servers**: singleton facades that own all of their state,
address every object through opaque generational handles, and have no knowledge of the ECS
world, the scene graph, or scripting.

Servers: `RenderServer`, `PhysicsServer`, `AudioServer`, `NavigationServer`, `TextServer`,
`DisplayServer`, `InputServer`.

Scene nodes and ECS components SHALL be thin front-ends that hold handles and push changes to
servers. (Influence: Godot's server/RID split.)

#### Scenario: Component drives a server through a handle
- **WHEN** a `MeshRenderer` component becomes visible
- **THEN** it SHALL hold a `RenderInstanceHandle` obtained from `RenderServer` and push transform
  and visibility changes to it
- **AND** the server SHALL never dereference an ECS entity or a scene node

#### Scenario: Backend is selectable
- **WHEN** the configuration selects a backend for a server (e.g. `physics.backend = "jolt"`)
- **THEN** the corresponding registered factory SHALL construct it, falling back to a documented
  default and finally to a null implementation that keeps handle bookkeeping valid

#### Scenario: Server runs off the simulation thread
- **WHEN** a server is configured to run on its own thread
- **THEN** calls from other threads SHALL be marshalled through a command queue and applied at a
  defined synchronisation point

### Requirement: ECS core with a scene-graph façade
The engine SHALL maintain **two coherent views** of the same world:

- The **ECS core** (`src/ecs/`) is the authoritative runtime storage: entities are generational
  ids, component data lives in packed per-archetype chunks, and behaviour runs as scheduled
  systems over queries.
- The **scene graph** (`src/scene/`) is the authoring and scripting façade: a `Node` is a named,
  hierarchical handle onto an entity, giving designers and Swift code an object-oriented view.

Every `Node` SHALL correspond to exactly one entity. Not every entity needs a node: systems MAY
create node-less entities for particles, instanced props, and other bulk data.

#### Scenario: Node is a view, not the storage
- **WHEN** Swift code reads `node.transform`
- **THEN** the value SHALL be read from the entity's `Transform` component in ECS storage, not
  from state duplicated in the node

#### Scenario: Bulk entities without nodes
- **WHEN** a system spawns 100 000 projectiles
- **THEN** they SHALL be created as entities with no scene nodes, and SHALL still be rendered,
  simulated, and culled

#### Scenario: The two views stay coherent
- **WHEN** an entity is destroyed by a system
- **THEN** its node, if any, SHALL be detached from the tree and invalidated before the next
  frame's scripting callbacks run

### Requirement: Module system
Optional functionality SHALL be packaged as **modules** under `modules/<name>/`, each providing
a `CMakeLists.txt`, a `module.toml` manifest (name, description, dependencies, default-enabled
flag, supported platforms), and registration entry points.

Modules SHALL register at one of four levels: `Core`, `Servers`, `Scene`, `Editor`.

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

### Requirement: Deterministic startup and shutdown
The runtime SHALL initialise subsystems in a fixed order and tear them down in exact reverse:

1. **Platform** — process setup, command line, logging, crash handler
2. **Core** — allocators, job system, type registry, virtual filesystem, configuration
3. Modules at level `Core`
4. **Display** — `DisplayServer`, window and surface creation
5. **Servers** — render device and `RenderServer`, `AudioServer`, `PhysicsServer`,
   `NavigationServer`, `TextServer`
6. Modules at level `Servers`
7. **ECS + Scene** — world creation, component and node type registration
8. Modules at level `Scene`
9. **Scripting** — ABI export table, Swift runtime, game module load
10. **Editor** (tools builds) and modules at level `Editor`
11. **Boot** — load the startup scene or project, enter the main loop

#### Scenario: Failure during startup unwinds cleanly
- **WHEN** any stage returns an error
- **THEN** all previously initialised stages SHALL be shut down in reverse order and the process
  SHALL exit with a diagnostic naming the failing stage

#### Scenario: Headless startup
- **WHEN** the runtime is started with `--headless`
- **THEN** the display and rendering stages SHALL construct null backends, and every other stage
  SHALL initialise unchanged

### Requirement: Main loop with fixed simulation and variable rendering
The runtime SHALL run zero or more fixed-step simulation ticks followed by exactly one
variable-step render per frame.

Per frame:

1. Sample the clock; accumulate elapsed time
2. Pump platform events and input
3. While the accumulator exceeds the fixed step, and up to `max_ticks_per_frame`:
   - begin tick: snapshot interpolatable transforms
   - run the `PreSimulation` system stage
   - run the `Physics` stage and step `PhysicsServer`
   - run the `Simulation` stage (gameplay systems, Swift `onFixedUpdate`)
   - run the `PostSimulation` stage; flush deferred structural ECS changes
4. Compute `interpolation_alpha` from the accumulator remainder
5. Run the `Frame` stage (variable-rate systems, Swift `onUpdate`, animation, UI)
6. Flush deferred structural changes
7. Run the `Render` stage: cull, build the render graph, submit
8. Present; run end-of-frame callbacks; run asset streaming and job-system maintenance

Defaults: fixed step 1/60 s, `max_ticks_per_frame` = 8. Exceeding the cap SHALL discard the
excess time so the loop cannot enter a death spiral.

#### Scenario: Slow frame is bounded
- **WHEN** a frame takes 400 ms
- **THEN** at most `max_ticks_per_frame` simulation ticks SHALL run and remaining accumulated
  time SHALL be discarded with a diagnostic counter incremented

#### Scenario: Smooth rendering between ticks
- **WHEN** a frame is rendered 40 % of the way between two simulation ticks
- **THEN** transforms marked interpolatable SHALL be rendered at the interpolated pose using
  `interpolation_alpha`

#### Scenario: Deterministic fixed-step mode
- **WHEN** the runtime is started with `--fixed-step <n>`
- **THEN** exactly one tick of fixed duration SHALL run per frame regardless of wall-clock time,
  producing reproducible simulation for recording and automated tests

### Requirement: Deferred command queue
The engine SHALL provide a frame-scoped deferred command queue for operations that are unsafe to
perform in the middle of a stage — entity creation and destruction, component add and remove,
node reparenting, and scene loading.

Deferred commands SHALL be applied at defined flush points (after each simulation stage and
after the frame stage), in submission order.

#### Scenario: Structural change during iteration
- **WHEN** a system iterating a query destroys an entity
- **THEN** the destruction SHALL be recorded and applied at the next flush point, and the
  in-progress iteration SHALL remain valid

### Requirement: Build-time feature slicing
The build SHALL allow whole subsystems to be compiled out through CMake options, each defining a
guard macro: `CY_EDITOR`, `CY_PHYSICS`, `CY_NAVIGATION`, `CY_AUDIO`, `CY_UI`, `CY_XR`,
`CY_SCRIPTING`, `CY_RENDERER_VULKAN`, `CY_RENDERER_METAL`, `CY_DEVELOPMENT` (assertions,
diagnostics, hot reload), `CY_PROFILING`.

#### Scenario: Dedicated server build
- **WHEN** configured with rendering, audio, UI, and editor disabled
- **THEN** the binary SHALL link no graphics or audio backend, and the render and audio servers
  SHALL be null implementations that preserve handle semantics

### Requirement: Non-goals for the initial architecture
The following are explicitly **out of scope** for the first architecture and SHALL be recorded as
deferred decisions rather than silently assumed:

- C++20 modules as the canonical interface form
- A visual scripting graph language
- Distributed or server-authoritative simulation beyond the replication model in
  `networking-and-replication`
- Console platform support
- Runtime engine-source compilation (the engine ships as a library, games ship as modules)

#### Scenario: Deferred decision is revisited
- **WHEN** a proposal requires one of these
- **THEN** it SHALL go through the OpenSpec change flow and update this list, rather than
  introducing the capability implicitly
