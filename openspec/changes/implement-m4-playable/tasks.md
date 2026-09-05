# Tasks: M4 — Playable

Ordered. The hot-reload spike first, because M5 is priced on its answer. Then the ABI, because
everything else crosses it. Then the subsystems, then the sample.

## 0. Spike — hot reload across the ABI with live Swift objects

M4's named risk (`design.md` §6). Its only deliverable is a decision.

- [ ] 0.1 Load a Swift module over the ABI, create behaviours holding live state
- [ ] 0.2 Edit, rebuild and reload the module; confirm state survives
- [ ] 0.3 Confirm stale function pointers are never called, and that a type whose layout changed is
      handled rather than reinterpreted
- [ ] 0.4 If state cannot be preserved, **STOP** and propose the roadmap change before section 2
      proceeds — M5's live editing depends on this answer

## 1. Carried forward from M3

- [ ] 1.1 `env-doctor` reports the Swift toolchain, and diagnoses the `~/.profile` PATH case
      specifically rather than reporting "swift: not found"
- [ ] 1.2 Record in `rhi-and-render-graph` that Vulkan validation does **not** police queue ownership
      transfers or memory aliasing — both must be structurally guaranteed, never tested for. M3's
      negative controls proved it and M7 is where it bites.
- [ ] 1.3 A device suite that renders **more than one frame**, so a per-frame defect cannot survive
      as it did at M3
- [ ] 1.4 `CY_SHADER_SLANG` — decide whether M3 delivered it; if so it defaults ON like the Vulkan
      backend, and if not the option says which milestone owns it

## 2. The ABI — `native-abi` → Working

- [ ] 2.1 The flat C interface: opaque handles, POD structs, `cy_`/`Cy`/`CY_` naming
- [ ] 2.2 The versioned interface table, append-only
- [ ] 2.3 **The compatibility gate** (`design.md` §1), landing with the first symbol. Prove it:
      reorder an entry and watch the build stop; remove one; append one and watch it pass.
- [ ] 2.4 Module entry points; type registration from modules
- [ ] 2.5 Value marshalling; errors cross as codes, never as exceptions
- [ ] 2.6 Callbacks into modules
- [ ] 2.7 Hot reload, per the spike's outcome
- [ ] 2.8 ABI compatibility testing in CI

## 3. Swift — `swift-scripting` → Working

- [ ] 3.1 `CyberdyneKit` **generated** from the ABI description (`design.md` §2)
- [ ] 3.2 The behaviour programming model
- [ ] 3.3 The system programming model, over the ECS's access declarations
- [ ] 3.4 Declarative registration via macros
- [ ] 3.5 Memory and lifetime rules; ARC across the boundary
- [ ] 3.6 Concurrency rules
- [ ] 3.7 Hot reload from the Swift side
- [ ] 3.8 Debugging and diagnostics; Swift API tests in CI
- [ ] 3.9 The engine core links no Swift runtime — proven, not asserted

## 4. Input, camera, physics, audio

### 4.1 `input-and-actions` → Working

- [ ] 4.1.1 Input users and device ownership; device lifecycle
- [ ] 4.1.2 Actions and value types; mapping contexts; bindings and composites
- [ ] 4.1.3 Processors, modifiers, triggers and the action lifecycle
- [ ] 4.1.4 **Fixed-tick sampling and buffering** (`design.md` §5) — a press and release between two
      ticks is observable by the next tick. Write that test first.
- [ ] 4.1.5 Control schemes and device detection; rebinding
- [ ] 4.1.6 Input diagnostics

### 4.2 `physics` → Working

- [ ] 4.2.1 `PhysicsServer` **before Jolt** (`design.md` §4), with the trivial implementation retained
- [ ] 4.2.2 Jolt behind it, pinned in `deps/manifest.toml`; no Jolt type above `src/backends/`
- [ ] 4.2.3 Physics components; fixed-step integration on the simulation clock
- [ ] 4.2.4 Collision events and filtering; queries
- [ ] 4.2.5 The character controller
- [ ] 4.2.6 Determinism across runs on one platform

### 4.3 `camera-system` and `audio` → Seed

- [ ] 4.3.1 The four separated concepts; a camera is not a scene object
- [ ] 4.3.2 The camera stack; follow and orbit; the lens model
- [ ] 4.3.3 Render view production, feeding M3's renderer
- [ ] 4.3.4 The audio driver layer over miniaudio, interface first
- [ ] 4.3.5 The bus graph; playback; basic spatialisation

### 4.4 `gameplay-framework` → Seed — **invariant, M4**

- [ ] 4.4.1 Gameplay lifetime and the gameplay context
- [ ] 4.4.2 Control sources and bindings
- [ ] 4.4.3 **One validated command stream** (`design.md` §3) — the simulation's only input
- [ ] 4.4.4 Command validation returns reasons
- [ ] 4.4.5 Deterministic random streams over M2's seeded streams
- [ ] 4.4.6 **The test that bypasses the stream, and fails**

## 5. The artefact

- [ ] 5.1 `samples/04-character` — a third-person character controller **written entirely in Swift**:
      move, jump, collide with a level, hear footsteps, followed by a camera
- [ ] 5.2 `just run-sample character`
- [ ] 5.3 A check that the sample contains **no C++ gameplay code**
- [ ] 5.4 Smoke test: headless, frame-limited, clean exit

## 6. Closing the milestone

- [ ] 6.1 The sample contains no C++ gameplay code
- [ ] 6.2 The ABI gate rejects a reordered entry and a removed one, and accepts an appended one
- [ ] 6.3 A Swift module hot-reloads while the sample runs, preserving world state
- [ ] 6.4 A test that bypasses the command stream fails
- [ ] 6.5 Physics is deterministic across runs on one platform
- [ ] 6.6 A press and release between two ticks is observed by the following tick
- [ ] 6.7 Swift API tests run in CI
- [ ] 6.8 All four profiles build clean and `just test-all` is green in each
- [ ] 6.9 Sanitizers green over the new suites
- [ ] 6.10 `just roadmap-milestone m4` exits zero, and m3, m2, m1 and m0 still do
- [ ] 6.11 Update `status.yaml` and `capability-matrix.md`; record what is thinner than the tasks claim
- [ ] 6.12 `openspec validate --specs --strict` passes; archive this change
- [ ] 6.13 Open the M5 change — the live bridge's latency is its named spike
