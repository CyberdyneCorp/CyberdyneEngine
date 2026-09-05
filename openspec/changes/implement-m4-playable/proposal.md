# Implement M4 — Playable: a game, written in Swift, over a boundary that will not move

## Why

M3 drew a frame. M4 makes it a game you can write — and it opens the boundary that every future
language binding, every plugin, and the entire editor will be built on.

**The first published ABI symbol starts a compatibility obligation that never ends.** The table is
versioned and append-only, and the CI gate that enforces it has to exist before the first consumer,
not after. An ABI that has been reordered once is an ABI nobody can trust, and the cost is paid by
every module compiled against it — which by M5 includes the editor.

**One validated command stream is the second thing that cannot be added later.** Replay, rollback
and lockstep are not three mechanisms; they are one command log read three ways, which is only true
if the simulation has exactly one input path. A second path — a system that reads input directly, a
tool that pokes state — does not merely bypass the stream, it makes the M9 guarantees unachievable
without finding and removing it. M4 is where input first reaches simulation, so M4 is where the rule
binds.

The third reason is ergonomic and it is the point of the milestone: **gameplay should be written in
Swift, not C++.** A character controller that moves, jumps, collides and is heard, with no C++ in
the project, is the proof that the boundary is real rather than aspirational.

## What Changes

- **The C ABI.** A flat, versioned, append-only interface with opaque handles and POD structs; module
  entry points; `cy::Expected` mapped to error codes rather than exceptions; callbacks into modules;
  type registration from modules; hot reload; and the **ABI compatibility gate** in CI.
- **The Swift overlay.** `CyberdyneKit` generated from the ABI description so the two cannot drift;
  behaviours and systems; the macros that make both declarative; ARC and concurrency rules; hot
  reload; and debugging that works.
- **Input.** Users and devices, actions, mapping contexts, bindings, processors and triggers, with
  fixed-tick sampling and buffering so a press between two ticks is not lost.
- **Camera at Seed.** The four separated concepts, a camera stack, follow and orbit, the lens model,
  and render view production feeding M3's renderer.
- **Physics.** `PhysicsServer` with Jolt behind it, components, fixed-step integration, collision
  events and filtering, queries, and the character controller.
- **Audio at Seed.** The driver layer over miniaudio, the bus graph, playback, spatialisation.
- **The gameplay command stream at Seed** — control sources and bindings, command validation that
  returns reasons, deterministic random streams, and the rule that the simulation has one input.

**Closing artefact**: `samples/04-character` — a third-person character controller written entirely
in Swift: move, jump, collide with a level, hear footsteps, follow with a camera.

## Capabilities

### Advanced Capabilities

`native-abi`, `swift-scripting`, `input-and-actions` and `physics` to **Working**;
`camera-system`, `audio` and `gameplay-framework` to **Seed**;
`core-platform-abstraction` to **Working** as input devices arrive.

### Modified Capabilities

- `testing-and-quality` — **a test's time budget is a property of the machine, not of the test.** M3
  found four unit cases sitting on the 1 ms budget, failing about once in ten on an idle machine, and
  because `four-profiles` is run by three milestone ledgers that nest each other, a single such case
  produced roughly a dozen exposures per pull request. The taxonomy's budgets need to be stated as
  what they are — a guard against a test becoming an integration test by accident — with a defined
  tolerance for machine variance, rather than as a hard threshold that turns CPU noise into a red
  build.

## Impact

- **New code**: `src/abi/`, `bindings/swift/`, `src/servers/{physics,audio}/`,
  `src/backends/{physics-jolt,audio-miniaudio}/`, `src/gameplay/`, and `samples/04-character/`.
  First code at the ABI layer and the first Swift in the repository.
- **New dependencies**: Jolt and miniaudio, both already named in `thirdparty-dependencies`' intended
  set, each behind an engine-owned interface with the interface written first.
- **New toolchain**: Swift 6.3.3 is installed. Note that `swiftly` writes its environment line to
  `~/.profile`, which only login shells read — CI and any spawned build shell need it sourced
  explicitly, and `env-doctor` should say so rather than reporting "swift: not found".
- **New permanent gates**: the ABI compatibility check, Swift API tests, and a check that the sample
  contains no C++ gameplay code.
- **Carried forward from M3**: the gate's findings, including that Vulkan validation does **not**
  police queue ownership transfers or memory aliasing — both must be structurally guaranteed rather
  than tested for — and that every device suite in M3 rendered exactly one frame, which is how a
  per-frame descriptor defect survived to the artefact.
- **Risk**: hot reload across the ABI with live Swift objects. If reload cannot preserve state, M5's
  live-editing story changes shape, and it is cheaper to know that now.
