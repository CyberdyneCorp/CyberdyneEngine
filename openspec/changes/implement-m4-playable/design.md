# Design: M4 — Playable

## The M3 handoff

| | |
|---|---|
| Renderer | `src/servers/render/`, `src/rendering/`, `src/backends/rhi/{null,vulkan}` — a lit frame, GPU scene, deterministic submission. `CY_RENDERER_VULKAN` is now ON by default. |
| Loop | `src/runtime/` — N fixed ticks then one variable frame, interpolation alpha, one commit point per tick |
| World | `src/ecs/`, `src/scene/` — archetypes, the node façade, cooked scenes |
| Determinism | `src/core/determinism/` — clock, epochs, commit boundary, seeded streams, `Classified<>`, hierarchical hashing |
| Reflection | `src/core/reflect/` + `identity/manifest.toml` — stable identifiers with a CI gate |
| Servers | `servers.h` — Render is registered; Physics, Audio, Navigation, Text and Input still resolve to `NullServer` |

**Swift is installed but not on `PATH`.** `swiftly` wrote its environment line to `~/.profile`,
which only login shells read; agent shells and CI do not. Every Swift invocation must source it:
`bash -lc '. ~/.local/share/swiftly/env.sh; swiftc …'`. `env-doctor` should say exactly that rather
than reporting "swift: not found", because the toolchain is present and the diagnosis is a PATH
problem, not a missing install.

## 1 — The ABI gate exists before the first consumer

`native-abi` requires a versioned, append-only interface table with a compatibility gate.

**Decision.** The gate lands with the **first exported symbol**, not with the first consumer.

The obligation starts the moment anything links against the table. An ABI that has been reordered
once is an ABI nobody can trust, and by M5 the editor is compiled against it — so the window in
which a reordering is cheap is this milestone and no other. The gate compares the current table
against a committed baseline and fails on anything but an append.

Test it the way M1's identity gate was tested: reorder an entry and watch the build stop, remove one
and watch it stop, append one and watch it pass. A gate that has never failed is a gate nobody knows
works.

## 2 — The overlay is generated, for the reason the manifest is

`swift-scripting` requires `CyberdyneKit` to be a generated overlay so that it cannot drift from the
ABI.

**Decision.** Generated from the same ABI description the gate checks, in the same build step that
produces the Rust SDK at M5. Hand-writing the overlay would reproduce, in a second language, exactly
the drift the reflection generator exists to prevent — and this project already has the argument
written down in `core-type-system`: a declaration that can drift from the thing it describes will.

## 3 — One command stream, and the second path is the failure

`gameplay-framework` requires one validated command stream as the simulation's only input.

**Decision.** Input reaches simulation **only** as commands. There is no "read the input state in a
system" path, and no tool that pokes simulation state directly.

This is the invariant the roadmap pins to M4, and it is worth being precise about why. Replay,
rollback and lockstep are not three mechanisms — they are one command log read three ways. That is
only true if the log is complete. A single system that reads a device directly does not merely
bypass the stream; it makes the M9 guarantees **unachievable** until someone finds and removes it,
and nothing will point at it, because everything will appear to work until a desync months later.

Write the test that bypasses the stream and fails.

## 4 — Interfaces before libraries

`thirdparty-dependencies` requires each integration behind an engine-owned interface.

**Decision.** `PhysicsServer` and the audio driver layer are defined and exercised by a trivial
implementation **before** Jolt and miniaudio are linked. M0 established the pattern with
`DisplayServer` before SDL3, and M3 with the RHI before Vulkan — the null backend written first is
why the RHI is an interface rather than a wrapper.

The retained trivial implementation is not ceremony: it is what proves at every build that the
interface does not leak the library. M3's gate found the inverse case worth remembering —
`CY_RENDERER_VULKAN` off by default meant the *real* backend was the one nothing tested.

## 5 — Fixed-tick input is where the subtle bug lives

`input-and-actions` requires that input consumed by fixed-step simulation is accumulated between
ticks, so no press is lost when a frame contains zero or several ticks.

**Decision.** The platform layer delivers timestamped events without coalescing transitions, and the
action layer resolves them per tick. A button pressed and released **between** two ticks must still
be observable as both a press and a release by the tick that follows.

This is the requirement most likely to be implemented as "sample the current state each tick", which
works in every manual test and loses inputs precisely when the frame rate is uneven — which is when
players notice.

## 6 — Hot reload across the ABI — the spike

M4's named risk. Swift objects hold state; a reloaded module must not orphan or corrupt it.

If reload cannot preserve state, M5's live-editing story changes shape — the editor's whole value
proposition is that iteration does not cost a restart. Knowing that now is worth a day; discovering
it at M5 is worth a milestone.

The spike loads a Swift module, creates behaviours with live state, edits and rebuilds the module,
reloads it, and checks that state survived, that stale function pointers are not called, and that a
type whose layout changed is handled rather than reinterpreted. Set `proceed=false` if the answer is
no.

## 7 — What M4 deliberately does not do

- **No editor.** M5. The sample is a standalone application.
- **No animation, no AI, no navigation.** M8. The character is a capsule that moves.
- **No networking, no replay, no rollback.** M9. The command stream lands here; what reads it later does not.
- **No abilities, no gameplay graphs.** M8.
- **No spatial acoustics.** Audio is Seed: the driver layer, the bus graph, playback.
