# `bindings/swift/` — the `CyberdyneKit` Swift package

Swift is CyberdyneEngine's gameplay scripting language, and this is the whole of its surface. A game
is an ordinary Swift package that depends on `CyberdyneKit` and produces a dynamic library the engine
loads as a module. Nothing in the engine links Swift; everything crosses `src/abi/include/cy/abi/cy_abi.h`.

**Governed by**: `swift-scripting`, and `native-abi` for the boundary itself.

## The four targets, and why the split is where it is

| Target | Written by | Contents |
|---|---|---|
| `CyberdyneABI` | **the generator** | The C ABI header, copied verbatim, plus a module map. Its one C source compiles that header *as C* on every build. |
| `CyberdyneCore` | **the generator** | The overlay: `Status`, `VarType`, `InitLevel`, `Vec2`–`Quat`, `Interface`, and the `Engine`/`World`/`BehaviourType` handle wrappers. |
| `CyberdyneKit` | by hand | `Behaviour`, `@Export`, the blob, the module entry point, components, systems, `@GameActor`, logging. |
| `CyberdyneMacros` | by hand | The compiler plugin behind `@Behaviour`, `@Component`, `@System` and `@GameModule`. |

`CyberdyneCore` is generated for the reason `design.md` §2 gives and `core-type-system` gave first:
*a declaration that can drift from the thing it describes will.* Hand-writing the overlay would
reproduce, in a second language, exactly the drift the reflection generator exists to prevent — and
the failure mode is worse here, because a Swift `struct` whose member order disagrees with the C
struct it is passed as does not fail to compile. It reads the wrong bytes.

```
just generate-swift            regenerate from src/abi/include/cy/abi/cy_abi.h
just generate-swift --check    fail if the committed overlay is stale, naming the file
```

The overlay is **committed**, so a consumer needs no generator. `integration.swift_overlay` runs the
check under `just test-all`, on every machine, with or without a Swift toolchain.

`VfxEffects.set(_:on:parameter:emitter:)` and `VfxEffects.get(from:parameter:emitter:)` use the
generated ABI 1.4 entries to change or read an exposed parameter on one playing scene effect.
Pass an empty emitter for a system parameter, or the owning emitter's name for a local parameter.
The entity is the effect entity in the Play world. Values use `Value.f32`, `vec2`, `vec3`, `vec4`,
`i64`, or `bool` according to the authored declaration; a mismatched type is refused.

## Writing a game

```swift
import CyberdyneKit

@Behaviour(schema: 1)
final class PlayerController: Behaviour {
    @Export var speed: Float = 6.0
    @Export(range: 0 ... 20) var jumpVelocity: Float = 8.0

    private var velocity = Vec3.zero          // not exported: rebuilt on reload

    override func onFixedUpdate(_ delta: Double) throws {
        velocity.y += -9.81 * Float(delta)
    }
}

@GameModule
enum Game: GameModule {
    static let behaviours: [any BehaviourClass.Type] = [PlayerController.self]
}
```

`@GameModule` emits the two `@_cdecl` entry points the loader looks for. They have to be in the
game's own module — a linker drops an unreferenced object out of a static archive, so an entry point
living in `CyberdyneKit` would be absent from the game's `.so` and the loader would report a module
that "did not export its declared entry symbol" with nothing pointing at why.

### The two programming models

**Behaviours** suit hand-authored gameplay objects: a class attached to an entity, with a lifecycle.
**Systems** suit bulk data: a function whose `Query<Write<Velocity>, Read<Mass>, Without<Grounded>>`
parameter *is* its access declaration, so the scheduler orders it against native systems by the same
conflict rules. Both may be used in one project.

The query is the declaration, and that is M1's own finding rather than a preference —
`src/ecs/include/cy/ecs/system.h`: "a system that writes down its access separately from the query it
runs can drift, and nothing catches the drift, because a declaration is only checked against other
declarations."

## Hot reload

Edit, rebuild, reload; `@Export`ed state survives. It does **not** survive in place — it survives by
serialize → migrate-by-name → recreate, which is what M4's spike measured over forty cycles including
a type-layout change. The blob format and the three properties that make it work are in
`Sources/CyberdyneKit/Serialization.swift`; the reload sequence and the reason no image is ever
unloaded are in `src/abi/include/cy/abi/module.h`.

Two rules a build must honour, because the loader cannot:

* **a different file per generation** — `dlopen` of a path already open returns the same image;
* **a different Swift `-module-name` per generation** — name-based type lookup is process-global and
  first-registration-wins, so two resident images called `CyGame` make the new one find the old one's
  metadata. Measured, with both addresses printed.

`tools/cy_swift_module.py` does both. It writes a small package whose target is `CyGame_g<N>` and
builds it with SwiftPM, so the toolchain is the standard one and only the target name is arranged.

## Debugging

Standard tooling, with no engine-specific setup — `swift-scripting` asks for exactly that. What
follows was run on this machine against `build/swift`, not inferred from how Swift usually behaves.

```
$ lldb --batch -o "breakpoint set --name '$s9CyGame_g012SwiftCounterC13onFixedUpdateyySdF'" \
       -o run -o "frame variable" -- ./cy_test_integration_swift_reload
```

```
* thread #1, stop reason = breakpoint 1.1
    frame #0: libCyGame_g0.so`SwiftCounter.onFixedUpdate(delta=0.01666666753590107)
              at Counter.swift:18:15
   17      override func onFixedUpdate(_ delta: Double) {
-> 18          ticks += 1
(lldb) frame variable
(Double) delta = 0.01666666753590107
(CyGame_g0.SwiftCounter) self = 0x00005555555c2450 {
  CyberdyneKit.Behaviour = { entity = (bits = 0)  isEnabled = true }
  _health = { wrappedValue = 95  exportedConstraint = none }
  _label  = { wrappedValue = "player"  exportedConstraint = none }
  ticks = 0
}
```

Source-level stepping, Swift locals, the `@Export` wrappers' storage and a `String` read as a
`String` — the specification's "Breakpoint in a behaviour" scenario, met.

Three things worth knowing, each of which cost a wrong reading before it was written down:

* **Use `lldb` from the Swift toolchain**, not the system one, and reach it the same way as every
  other Swift command: `bash -lc '. ~/.local/share/swiftly/env.sh; lldb …'`. The system `lldb` has no
  Swift language plugin and prints the object as raw words.
* **Set the breakpoint by symbol, not by file.** A game module is `dlopen`ed, so at launch there is
  no `Counter.swift` for `breakpoint set --file` to resolve against and the pending breakpoint stays
  pending — it does not resolve when the image arrives. `--name` with the mangled symbol resolves the
  moment the module loads (`1 location added to breakpoint 1`), and `nm libCyGame_g<N>.so` is where
  the symbol comes from. The generation is *in* the symbol, which is the module-name rule paying off
  a second time: a breakpoint names the generation it belongs to.
* **`SWIFT_BACKTRACE=enable=no`** in any harness, or a crashing case buries its own failure under
  forty lines of backtrace.

Behaviour failures do not need a debugger. A thrown error is caught by the bridge, logged with the
behaviour and the callback that produced it, and the instance is disabled; `Log.info`/`.warning`/
`.error` go into the engine's diagnostic stream, not stdout, so they land in the same timeline as
everything else. A Swift **trap** is still fatal — see the list below.

## Layout

```
Package.swift               the manifest; swift-syntax is its one dependency, pinned exactly
Sources/CyberdyneABI/       generated: the C header and its module map
Sources/CyberdyneCore/      generated: the overlay
Sources/CyberdyneKit/       hand-written: the ergonomic layer
Sources/CyberdyneMacros/    hand-written: the macro plugin
Tests/                      161 cases; CyberdyneCoreTests/Generated/ is generated too
fixtures/reload/            two generations of one module, for the reload suite
tests/                      the C++ side of the reload suite
tools/                      the module builder and the no-Swift-runtime check
CMakeLists.txt              registers the checks as CTest entries; builds no Swift
```

## What runs under `just test-all`

| Test | Needs Swift? | What it holds |
|---|---|---|
| `integration.swift_overlay` | no | the committed overlay is what regeneration produces |
| `integration.swift_overlay_gen` | no | the generator refuses an ABI it does not fully cover |
| `integration.swift_no_runtime` | no | **task 3.9** — no engine binary links or embeds Swift |
| `integration.swift_package` | yes | `swift test`: the package's own cases |
| `integration.swift_reload` | yes | a real Swift module, hot-reloaded by the engine's loader |

Without a Swift toolchain the last two are **not registered**, and the configure says so. When a
toolchain is present, both are registered. `integration.swift_package` runs the real `swift test`
command so SwiftPM resolves XCTest through the package test environment; if that environment is
incomplete, the suite fails visibly instead of disappearing from the test list.

`swiftly` writes its environment line to `~/.profile`, which only login shells read. Every Swift
invocation here goes through `bash -lc '. <env.sh>; …'`; `just env-doctor` diagnoses the case where
it has not been sourced.

## The game services (ABI 1.3)

`add-swift-game-api` appends input, camera, physics queries, navigation, audio, spawning and time to
the table, so a game calls them instead of writing components for a C++ host to carry
(`samples/04-character/game/Contract.swift` still does the latter, and migrates when the services
are implemented). `CyberdyneCore` already has one throwing `Engine` method per entry and the enums
`Phase`, `ShapeKind`, `NavPathStatus` and `NavQueryState`, all generated.

`CyberdyneKit` gets one facade per service — `Input`, `Camera`, `Physics`, `Navigation`, `Audio`,
`Spawn`, `Time` — each in its own file, over the shared `Pose`, `Ray` and tuple conversions in
`GameTypes.swift`. A call made in a phase its entry refuses throws
`CyberdyneError.status(.permissionDenied, …)`; with no engine bound, `.unavailable`. The facades are
tested through the package with `Tests/CyberdyneKitTests/FakeEngine.swift`, an interface table built
in Swift. The phase and determinism rules are `openspec/changes/add-swift-game-api/design.md`'s.

### Audio, spawning and time

```swift
try Audio.play("explosion", at: impact)                          // any phase; fire and forget
let hum = try Audio.play(Audio.cue("tank.engine"), attachedTo: tank,
                         options: PlayOptions(loop: true, fadeIn: 0.3))
try hum?.stop(fadeOut: 0.5)
try Audio.bus("Music").setVolume(0.2, fade: 2)

let tank = try World.spawn(prefab: "units/tank", at: Pose(position: p))   // fixed step or init
let squad = try Spawn.prefab("units/rifleman").instantiate(at: formation, parent: tank)
try World.destroy(tank)                                          // the subtree, children first

let tick = try Time.now.tick                                     // any phase
```

* **Audio is presentation**, callable in every phase. While a tick is being resimulated `play`,
  `stop` and `setVolume` succeed and do nothing, and `play` returns nil — as it does when the mixer
  has no voice to give. A fixed step must not branch on `Voice.isPlaying`.
* **Spawning is simulation**: `instantiate` and `destroy` are refused in `onUpdate` with
  `.permissionDenied`. `Spawn.prefab(_:)` loads an asset only at initialisation; resolve prefabs in
  `onCreate` and keep the `Prefab`, and a fixed step never stalls on a load. The same calls in the
  same fixed step give the same entities on every run.
* **`Time.now` in `onFixedUpdate`** reports `frameDelta` and `interpolation` as zero, so a fixed
  step cannot come to depend on the frame rate.
* **`onUpdate` is driven.** A behaviour that overrides it registers `frame_update`, and the engine's
  `BehaviourRuntime::frame_update` calls it in the frame-update phase; one that does not registers
  nothing and is never scheduled for a frame.

### Writing an RTS unit in Swift

`samples/13-rts-api` is the whole of this, runnable (`just run-sample rts-api`) and tested
(`integration.rts_api_sample`). The shape, from its `game/Commander.swift`:

```swift
@Behaviour(name: "Commander", schema: 1)
final class Commander: Behaviour {
    @Export(range: 0.5...20) var unitSpeed: Float = 6
    private var worker = Prefab(raw: 0)
    private var squad: [Entity] = []
    private var selected: Entity = .null
    private var pendingOrder: Vec3?

    // No phase: the only place a prefab may load. Resolve once, keep the handle.
    override func onCreate() throws {
        worker = try Spawn.prefab("units/worker")
        for unit in try worker.instantiate(at: [Pose(position: a), Pose(position: b)]) {
            try NavAgent(unit).configure(.init(radius: 0.5, height: 1.8, maxSpeed: unitSpeed))
            squad.append(unit)
        }
    }

    // Frame: device and camera state. Read the click, decide what it means, RECORD it.
    override func onUpdate(_ delta: Double) throws {
        guard let camera = try Camera.active() else { return }
        let pointer = try Input.pointer()
        if pointer.pressed.contains(.left) {
            let hit = try Physics.raycast(camera.ray(under: pointer), filter: .init(mask: 1 << 1))
            selected = hit?.entity ?? .null
        }
        if pointer.pressed.contains(.right),
            let ground = try Physics.raycast(camera.ray(under: pointer), filter: .init(mask: 1 << 0))
        {
            pendingOrder = ground.point
        }
    }

    // Fixed step: simulation. ACT on the recorded order; hear arrivals; build.
    override func onFixedUpdate(_ delta: Double) throws {
        if let target = pendingOrder, !selected.isNull {
            try NavAgent(selected).move(to: target)
            pendingOrder = nil
        }
        for unit in squad {
            let state = try NavAgent(unit).state
            if state.justArrived {
                try Audio.play("unit.arrived", at: state.position)
            }
        }
        if try Input.action("unit.spawn").justPressed {
            try squad.append(worker.instantiate(at: Pose(position: barracks)))
        }
    }
}
```

Four rules make it work, and each is one the engine enforces rather than one the game remembers:

* **Read devices in `onUpdate`, change the simulation in `onFixedUpdate`.** `Input.pointer()`,
  `Input.modifiers()` and every camera read throw `.permissionDenied` in a fixed step, and
  `instantiate`, `destroy`, `NavAgent.move(to:)` and `requestPath` throw it in a frame. A click
  becomes a value the behaviour keeps and the next fixed step consumes. Action state
  (`Input.action(_:)`) is resolved per tick, so it is readable in both.
* **Ask physics for one layer.** The host decides what is on which collision layer; the game picks
  with `Physics.Filter(mask:)`. A unit click asks for units only, so the ground under a unit never
  wins, and an order asks for the ground only, so a unit in the way does not become the target.
* **"Has it arrived" is an event, not a distance.** `NavAgent.state.justArrived` is true in exactly
  one fixed step, so a cue plays once however many ticks the unit then stands there.
* **Resolve names once.** `Spawn.prefab`, `Audio.cue` and `Input.find(action:)` return handles that
  stay valid across a hot reload; a fixed step that resolves by name each tick pays a lookup for a
  number that cannot change.

The camera is presentation: `camera.setTarget(CameraTarget(focus: .position(focus)))` from
`onUpdate` moves the focus of whatever rig the host built, and shows on the next frame. Yaw,
pitch and distance are recorded for the host's rig to apply (`CameraAdapter::framing()`): the camera
server has orbit intents and no absolute orbit, so a target's angle does not move the camera on its
own.
`samples/13-rts-api/game/RtsCamera.swift` pans that focus with the keys and the screen edges.

What the host still does, and why none of it is gameplay: it builds the level (ground, navigation
tile, prefab, cue, input actions) and names each piece, binds the adapters, and gives every
navigation agent a body and a scene node to move. See `samples/13-rts-api/README.md`.

## Scheduled systems, the tree, bodies and characters (ABI 1.5)

`add-swift-m12-gaps` appends what M12's RTS could not do without:

* **`@System` runs in the engine's scheduler.** A `GameModule` lists `components` (registered first)
  and `systems` (`[__CySystem_trainUnits.self]`); registering one in a bound module calls
  `register_system` with the query's terms as component ids and the attribute's stage, and the host's
  `cy::abi::ScriptSystems` installs it beside native systems, ordered by the same conflict rules, run
  in its stage's phase. The body gets `EngineChunkSource`: `world_chunks` per component, joined by
  archetype, `Without` archetypes skipped. Structural calls throw `.unavailable` while it runs; a
  reload that changes a system's stage or access is refused. `Systems.swift` says how.
* **The tree callbacks are driven.** `CyBehaviourVTable` gained `enter_tree`, `ready`, `enable`,
  `disable` and `exit_tree`; the bridge registers each only for a class that wrote it. A behaviour
  attached to a scene node (`cy::game_backend::ScriptSceneBridge`) receives them at the scene tree's
  pump, in the tree's order.
* **`@Node(path)` resolves at `onReady`**, through `node_find`, relative to the behaviour's node or
  absolute; a path that does not resolve is nil and a warning. `SceneTree.find(_:from:)` is the same
  lookup.
* **`RigidBody`** — `applyForce`, `applyImpulse(_:at:)`, `applyTorque`, `setVelocity`, `velocity` — and
  **`CharacterController`** — `create(on:_:)`, `move(velocity:jump:)` (one fixed step), `state`,
  `destroy()` — over the physics server and `cy::physics::CharacterController`.

Tested through `FakeEngine` in `SystemEngineTests.swift`, `TreeCallbackTests.swift` and
`BodiesTests.swift`; end to end in `samples/13-rts-api` (`integration.rts_api_sample`).

## What is thinner than `swift-scripting` asks for

Recorded here rather than only in a report, because these are the places a reader will look:

* **No resource in a system.** `Res<...>` as a system parameter is diagnosed by the macro and
  refused at registration: no entry reads a resource.
* **The tree callbacks need a node.** A behaviour created on a bare entity has no tree and receives
  none of the five; only one attached through the scene bridge (or spawned from a prefab node that
  names it) does.
* **No `async` wrappers.** The generator emits them for entries declared asynchronous, and no entry
  in the current table is; asset loading is the first one that will be.
* **A trap in game code is still fatal.** A *thrown* error is caught, logged with the behaviour and
  callback that produced it, and disables the instance. A Swift trap has no catch on any platform.
* **No shipping configuration.** `swift-scripting` asks for two: development (dynamic, hot-reloadable
  — this one) and shipping (optionally static, whole-module and cross-module optimisation, no dynamic
  load and no reload). The static path is untried; the `-O` half of it is exercised, because the
  Swift configuration follows the engine's profile and `--profile release` builds the fixture at
  `swiftc -O`.
* **The Swift toolchain version is not pinned.** `swift-scripting` requires a pin "per engine release
  and verified in CI"; nothing in the repository owns one. `deps/host-tools.toml` is where it belongs,
  and `just env-doctor`'s Swift check already says it will read a `swift` entry from there when one
  lands.
* **`swift build` and `swift test` are exercised; Xcode and SourceKit-LSP are not.** The package is
  an ordinary one with no custom toolchain, which is what the scenario asks for, but only the two
  command-line halves have been run — this is a Linux machine.
* **Linux only.** Every measurement here is on Linux with Swift 6.3.3. The macOS and Windows loader
  paths, the module-name rule, and `@_cdecl` export behaviour are **unverified** on those platforms.

## For whoever wires the milestone up

* **`.github/workflows/ci.yml` needs a Swift toolchain** for `integration.swift_package` and
  `integration.swift_reload` to run there. Without one they are not registered and CI runs the other
  three Swift checks only. A `swift-actions/setup-swift` step (or `swiftly`) in the `test` job's
  Linux legs is the whole of it; the recipes need no change, because every Swift invocation already
  goes through a login shell that sources the toolchain's environment.
* **`CY_SCRIPTING` is still `OFF` in `cmake/features.cmake`, and nothing here is behind it.** The
  Swift checks are gated on the *toolchain being present*, not on an option, so on a machine with
  Swift they are on by default — which is what rule 3 is for. Flipping `CY_SCRIPTING` to
  `DEVELOPMENT` is still worth doing, because `just env-doctor` reads that default and would then
  treat an absent Swift as a problem; it must land **together** with the CI step above, or every CI
  leg fails `env-doctor` on a runner with no Swift.
* **`Package.resolved` is gitignored** repository-wide, so `exact:` in `Package.swift` is the only
  thing pinning swift-syntax. Do not relax it to `from:`.
