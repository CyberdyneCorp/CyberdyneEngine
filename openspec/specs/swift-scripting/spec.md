# swift-scripting Specification

## Purpose

Defines Swift as CyberdyneEngine's gameplay scripting language: the `CyberdyneKit` Swift package,
the generated overlay over the C ABI, the behaviour and system programming models, memory and
concurrency rules across the boundary, hot reload, debugging, and how Swift game code is built
and shipped.

Swift is chosen for gameplay because it is memory-safe, statically typed, compiled to native code
(no interpreter, no GC pauses — ARC instead), has modern ergonomics that suit gameplay code, and
has first-class tooling. The engine core stays C++20; Swift never becomes a dependency of the
core.

## Requirements

### Requirement: Swift is a consumer of the ABI, not a dependency of the core
The engine core SHALL NOT link against, require, or embed the Swift runtime. Swift support SHALL
be a module that loads Swift game code as a native module through the C ABI.

An engine build with `CY_SCRIPTING=OFF` SHALL be fully functional, driven by native systems.

#### Scenario: Engine without Swift
- **WHEN** the engine is built without scripting
- **THEN** no Swift runtime SHALL be linked and the binary SHALL contain no Swift symbols

#### Scenario: Swift runtime ships with the game
- **WHEN** a game using Swift is packaged
- **THEN** the Swift runtime libraries SHALL be bundled with the game, not with the engine core

### Requirement: CyberdyneKit package
Swift game code SHALL depend on a single Swift package, **`CyberdyneKit`**, composed of:

| Target | Contents |
|---|---|
| `CyberdyneABI` | A C target exposing the generated ABI headers |
| `CyberdyneCore` | Generated Swift overlay: math types, handles, components, servers |
| `CyberdyneKit` | Hand-written ergonomic layer: `Behaviour`, `System`, property wrappers, queries |
| `CyberdyneMacros` | Swift macros for declarative registration |

Games SHALL be ordinary Swift packages depending on `CyberdyneKit` and producing a dynamic
library that the engine loads as a module.

#### Scenario: Standard Swift tooling
- **WHEN** a developer opens a game package
- **THEN** `swift build`, `swift test`, Xcode, and VS Code with SourceKit-LSP SHALL work with no
  custom toolchain

### Requirement: Generated overlay
The overlay in `CyberdyneCore` SHALL be **generated** from the same ABI description that produces
the C headers, so the two cannot drift.

Generation SHALL produce:
- Swift `struct`s for math and POD types, layout-compatible with their C counterparts, marked
  `@frozen` where the layout is ABI-stable
- Typed wrappers over handles with computed properties calling typed ABI fast paths
- Swift `enum`s for ABI enums
- Throwing Swift functions wrapping `CyResult`-returning ABI calls
- `async` wrappers for engine operations that are inherently asynchronous (asset loading)

Generated code SHALL be committed to the repository so consumers do not need the generator, and
CI SHALL verify it is up to date.

#### Scenario: Layout compatibility is verified
- **WHEN** the generator emits a Swift struct for a C struct
- **THEN** generated tests SHALL assert matching size, alignment, and field offsets on every
  supported platform

#### Scenario: Stale generated code is caught
- **WHEN** the ABI changes without regenerating
- **THEN** CI SHALL fail with a diff between committed and freshly generated overlay

#### Scenario: Errors become Swift errors
- **WHEN** an ABI call returns a failure status
- **THEN** the overlay SHALL throw a typed `CyberdyneError` carrying the status and the engine's
  last-error message

### Requirement: Behaviour programming model
`Behaviour` SHALL be the ergonomic, object-oriented entry point: a Swift class attached to a
node, receiving lifecycle callbacks.

```swift
@Behaviour
final class PlayerController: Behaviour {
    @Export var speed: Float = 6.0
    @Export(range: 0...20) var jumpVelocity: Float = 8.0

    @Node("../Camera") var camera: CameraNode?

    private var velocity = Vec3.zero

    override func onReady() {
        node.name = "Player"
    }

    override func onFixedUpdate(_ dt: Double) {
        let move = Input.vector("move")
        velocity.x = move.x * speed
        velocity.z = move.y * speed
        if Input.justPressed("jump"), isGrounded {
            velocity.y = jumpVelocity
        }
        velocity.y += Physics.gravity * Float(dt)
        characterController.move(velocity * Float(dt))
    }
}
```

Behaviours SHALL support the lifecycle defined in `scene-graph-and-nodes`: `onCreate`,
`onEnterTree`, `onReady`, `onEnable`, `onDisable`, `onFixedUpdate`, `onUpdate`, `onExitTree`,
`onDestroy`.

Only overridden callbacks SHALL be dispatched.

#### Scenario: Unimplemented callback costs nothing
- **WHEN** a behaviour does not override `onUpdate`
- **THEN** it SHALL not be registered in the per-frame dispatch list

#### Scenario: Exported property in the inspector
- **WHEN** a property is marked `@Export`
- **THEN** it SHALL appear in the editor inspector with its type and constraints, be serialized
  with the scene, and be settable per instance

#### Scenario: Node reference resolution
- **WHEN** a property is marked `@Node(path)`
- **THEN** it SHALL be resolved at `onReady` and be `nil` if the path does not resolve, rather
  than trapping

### Requirement: System programming model
Swift SHALL also be able to define **systems** for data-oriented work, with access declared in
the signature so the scheduler can parallelise them exactly as it does native systems.

```swift
@System(stage: .simulation)
func applyGravity(
    _ query: Query<Write<Velocity>, Read<Mass>, Without<Grounded>>,
    time: Res<Time>
) {
    for (velocity, mass) in query {
        velocity.linear.y -= 9.81 * Float(mass.value > 0 ? 1 : 0) * Float(time.fixedDelta)
    }
}
```

Query iteration SHALL operate over chunk-contiguous storage through borrowed pointers, so a
Swift system's inner loop does not marshal per entity.

#### Scenario: Swift system is scheduled like a native one
- **WHEN** a Swift system declares `Write<Velocity>` and a native system declares
  `Read<Velocity>`
- **THEN** the scheduler SHALL order them by the same conflict rules

#### Scenario: Bulk iteration does not marshal
- **WHEN** a Swift system iterates 100 000 entities
- **THEN** it SHALL access chunk arrays directly through borrowed buffers, with no `CyVar`
  conversion and no per-entity ABI call

#### Scenario: Guidance between models
- **WHEN** documentation describes the two models
- **THEN** it SHALL state that behaviours suit hand-authored gameplay objects and systems suit
  bulk data, and that both may be used in one project

### Requirement: Declarative registration via macros
Swift macros SHALL generate the registration code that the ABI requires, so developers do not
write boilerplate:

- `@Behaviour` — registers the class, its exported properties, and its overridden callbacks
- `@Component` — registers a Swift struct as an ECS component with field reflection
- `@System(stage:)` — registers a function as a system with access derived from its signature
- `@Export` — marks a property for inspector, serialization, and (optionally) replication
- `@Node(path)` — declares a resolved node reference

Macros SHALL emit compile-time diagnostics for invalid usage: non-representable exported types,
components with reference types, systems with conflicting access.

#### Scenario: Invalid component is rejected at compile time
- **WHEN** a struct marked `@Component` contains a Swift class reference
- **THEN** the macro SHALL emit a compile error explaining that components must be
  trivially relocatable value types

#### Scenario: Access derived from the signature
- **WHEN** a system takes `Query<Write<Health>>`
- **THEN** the macro SHALL register `Write` access for `Health` without a separate declaration

### Requirement: Memory and lifetime rules
Swift SHALL manage its own objects with ARC. Engine objects SHALL be referenced by handle, never
by imported C++ object lifetime.

The overlay SHALL enforce:
- A `Node` or entity wrapper is a **handle**, not an owning reference; holding one does not keep
  the entity alive
- Accessing a handle whose target is destroyed SHALL return `nil` or throw, never trap on freed
  memory
- Borrowed component pointers SHALL be scoped to the callback or iteration that produced them and
  SHALL NOT escape (enforced by `~Escapable` where the language allows, and by development-build
  checks otherwise)
- Retain cycles between behaviours SHALL be the developer's responsibility, as in any Swift code;
  the engine SHALL hold behaviours weakly from the entity side

#### Scenario: Destroyed entity accessed from Swift
- **WHEN** a behaviour holds a node handle whose entity was destroyed
- **THEN** accessing it SHALL return `nil` or throw a `CyberdyneError.invalidHandle`

#### Scenario: Escaping component pointer is caught
- **WHEN** a Swift system stores a borrowed component pointer beyond its iteration
- **THEN** development builds SHALL detect the escape at the next structural flush

### Requirement: Concurrency rules
Swift `async`/`await` SHALL be usable in game code for asset loading, timers, and sequencing, and
SHALL resume on the engine's scheduler rather than on an arbitrary Swift executor.

The overlay SHALL provide a `@MainActor`-equivalent **`@GameActor`** confining engine mutation to
the simulation thread. Swift structured concurrency tasks that touch engine state SHALL be
`@GameActor`-isolated.

Swift systems running on job workers SHALL NOT use `async`; they are synchronous functions
scheduled by the engine.

#### Scenario: Awaiting an asset load
- **WHEN** a behaviour awaits `Assets.load(Mesh.self, id)`
- **THEN** the task SHALL suspend, the load SHALL proceed on the asset thread, and the
  continuation SHALL resume on the simulation thread inside the game actor

#### Scenario: Unsynchronised mutation is rejected
- **WHEN** Swift code mutates engine state from a detached task without game-actor isolation
- **THEN** Swift's concurrency checking SHALL produce a compile-time error

### Requirement: Hot reload
Swift game modules SHALL be hot-reloadable in development builds, using the module reload
mechanism in `native-abi`.

On reload: `@Export` property values and behaviour state that is representable SHALL be
serialized, the module swapped, and state restored by name with schema migration.

#### Scenario: Edit-compile-see loop
- **WHEN** a developer edits a behaviour and rebuilds the game package
- **THEN** the editor SHALL detect the new library, reload it, and resume play with preserved
  entity and exported-property state

#### Scenario: Non-representable state is dropped
- **WHEN** a behaviour holds private state that cannot be serialized
- **THEN** it SHALL be reinitialised on reload and the behaviour SHALL be given a
  `onAfterReload` callback to rebuild it

### Requirement: Debugging and diagnostics
Swift game code SHALL be debuggable with standard tooling: LLDB attaching to the engine process
with Swift symbols, breakpoints in behaviours and systems, and source-level stepping.

Engine errors raised from Swift SHALL carry a Swift stack trace. The engine's profiler SHALL
attribute time to named Swift systems and behaviours.

#### Scenario: Breakpoint in a behaviour
- **WHEN** a breakpoint is set in `onFixedUpdate` and the engine runs under LLDB
- **THEN** execution SHALL stop with Swift locals inspectable

#### Scenario: Script error surfaces in the editor
- **WHEN** a Swift behaviour traps
- **THEN** the engine SHALL report the error with the Swift stack trace in the editor log and
  SHALL disable that behaviour rather than terminating the process, in development builds

### Requirement: Build and packaging
The build SHALL support two configurations for Swift game code:

- **Development** — built as a dynamic library, hot-reloadable, loaded at runtime
- **Shipping** — optionally statically linked, with whole-module optimisation and cross-module
  optimisation enabled

Supported Swift toolchain versions SHALL be pinned per engine release and verified in CI on
every supported platform.

#### Scenario: Shipping build has no dynamic module load
- **WHEN** a game is packaged for release with static linking
- **THEN** the module registration SHALL occur at startup with no dynamic library, and hot reload
  SHALL be absent

#### Scenario: Toolchain drift is caught
- **WHEN** a Swift release changes behaviour the overlay depends on
- **THEN** CI SHALL fail on the pinned-version matrix before the change reaches users

### Requirement: Other languages are not precluded
Because the boundary is a flat C ABI, additional language bindings (C, C++, Rust, C#) SHALL be
possible without engine changes.

Swift SHALL be the **officially supported** scripting language; others are community territory.

#### Scenario: Third-party binding
- **WHEN** a contributor writes a Rust binding against the ABI
- **THEN** it SHALL require no engine modification, and the ABI description file SHALL be
  sufficient to generate it
