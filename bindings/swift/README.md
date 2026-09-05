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
Tests/                      61 cases; CyberdyneCoreTests/Generated/ is generated too
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

Without a Swift toolchain the last two are **not registered**, and the configure says so — the same
arrangement `tests/render/` uses for a machine with no GPU, and for the same reason: a suite that is
registered and skips is a suite whose green means nothing.

`swiftly` writes its environment line to `~/.profile`, which only login shells read. Every Swift
invocation here goes through `bash -lc '. <env.sh>; …'`; `just env-doctor` diagnoses the case where
it has not been sourced.

## What is thinner than `swift-scripting` asks for

Recorded here rather than only in a report, because these are the places a reader will look:

* **No chunk source.** ABI 1.0's table has thirty entries and none of them hands a module a chunk.
  The system model, its access derivation, and an inner loop that does not marshal are complete and
  tested; the *source* of chunks is `ChunkSource`, a protocol waiting on an appended
  `world_query_chunks` entry. `Res<...>` as a system parameter is diagnosed for the same reason.
* **Two enums in this package are copies of engine enums the ABI does not carry.** `SystemStage` is
  `cy::ecs::Stage`'s values, with no `CyStage` to check them against; `Severity` is
  `cy::DiagnosticSeverity`'s, with no `CySeverity`. The second one *was already wrong* — six
  enumerators where the engine has three, so every `Log.info` arrived in the engine's log as an
  error, on a green run, for as long as nobody read the word — and the fix is
  `integration.swift_reload`'s "a behaviour's `Log.info` reaches the engine as Info" case, which
  installs a diagnostic sink and reads the severity the engine actually received. A Swift-side
  assertion could not have caught it: this side asserts what it *sent*. Appending `CyStage` and
  `CySeverity` to `CyInterface` would let the generator own both, and that is the real fix.
* **The tree callbacks are declared, not driven.** `CyBehaviourVTable` carries `create`, `destroy`,
  `fixed_update`, `serialize` and `deserialize`. `onEnterTree`, `onReady`, `onEnable`, `onDisable`,
  `onUpdate` and `onExitTree` are part of the model and recorded in `behaviourCallbacks`; the engine
  gains the thunks when the scene entries are appended.
* **`@Node(path)` resolves to nil.** There is no node entry in ABI 1.0. The wrapper is the seam.
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
