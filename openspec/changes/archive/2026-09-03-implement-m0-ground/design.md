# Design: M0 — Ground

The specifications settle most of M0. The layer table is in `engine-architecture`, the build
configurations and feature options in `build-system-and-platforms`, the recipe surface in
`developer-workflow-and-just`, the test taxonomy in `testing-and-quality`, the dependency set in
`thirdparty-dependencies`. This document records only what they leave open, and why each open
question is settled the way it is.

## The rule this milestone is built under

M0 implements the **structure** of seven capabilities and almost none of their behaviour. That is
the Seed tier, and the temptation it invites is to keep going — to write the containers because
`core-memory-and-containers` is right there, to add a job system because the trace wants worker
threads. The milestone gate is the defence: M0 closes on a window and a trace file, and work that
does not serve that artefact belongs to M1.

The inverse temptation is worse. Three things must be right in M0 because they are properties of
everything written afterwards, and each is called out below.

---

## 1 — Layering is a configure-time failure

`engine-architecture` fixes the layer order; `project-and-plugins` requires it enforced by the
project graph rather than by review.

**Decision.** Every target is declared through one CMake function, `cy_add_module()`, which takes a
layer and its dependencies. The function records the layer in a target property and refuses at
configure time to link a target whose layer is above its own. There is no way to add a target that
opts out — a bare `add_library` in this tree is itself a lint failure.

| Layer | Index | Directory |
|---|---:|---|
| Core | 0 | `src/core/` |
| ECS | 1 | `src/ecs/` |
| Servers | 2 | `src/servers/` |
| Backends | 3 | `src/backends/`, `platform/` |
| Scene | 4 | `src/scene/` |
| Runtime | 5 | `src/runtime/` |
| ABI | 6 | `src/abi/` |
| Editor / Tools | 7 | `editor/`, `tools/` |

Two checks, because target dependencies alone do not catch everything: the CMake function catches
declared links, and a source-level check catches an `#include` that reaches upward through a path
CMake never saw. The second is cheap to run and is what actually fires in practice.

**Why now.** The alternative — enforcing at M4 — means auditing four milestones of accumulated
includes. Cost now: a day. Cost later: unbounded, and it competes with work people actually want.

## 2 — Privacy classification is part of the field, not a review step

`diagnostics-profiling-and-crash` requires every diagnostic field to carry a privacy
classification, and the roadmap pins it to M0.

**Decision.** Classification is a required argument of the field macro, not a decoration. There is
no overload that omits it:

```cpp
CY_TRACE_FIELD(frame_index, u64,   cy::Privacy::Public)
CY_TRACE_FIELD(user_path,   string, cy::Privacy::Sensitive)
```

Redaction is applied by the trace writer according to the classification and the active export
policy, so a field cannot be exported at a level it was never classified for.

**Why now.** Classification added later is an audit of every field ever written, performed by
someone who did not write them. Making it un-omittable at the point the first field is declared
costs nothing and cannot decay.

## 3 — The runtime does not own the loop

`core-platform-abstraction` requires that a platform which drives frames itself — mobile, web — is
not precluded, and that `tick()` exists rather than an internal `while (running)`.

**Decision.** The runtime exposes `Runtime::tick()` from M0, and the desktop host is a thin caller
of it. The empty sample's loop lives in the host, not the engine.

**Why now.** This is four lines of difference in M0 and a restructure of the frame in M6, by which
point streaming, rendering and simulation all assume they are inside a loop the engine owns.

---

## 4 — SDL3 beneath the platform interfaces

`thirdparty-dependencies` names SDL3 for input and gamepads and leaves windowing open;
`core-platform-abstraction` specifies `DisplayServer` as an engine-owned interface that provides the
graphics surface without the backend containing platform `#ifdef`s.

**Decision.** SDL3 implements `Platform`, `DisplayServer` and the input event source for all three
desktop platforms, under `platform/desktop-sdl3/`. No SDL type appears above `platform/`, and a
build gate enforces it. A native backend for one desktop platform is scheduled at M11.

**Considered and rejected: native backends now.** Win32, Cocoa, X11 and Wayland written directly is
three to four times M0's platform work — Wayland alone is substantial — for a milestone whose
artefact is a window that opens and closes. It buys control over DPI, IME and surface details that
nothing in M0 through M10 needs.

**Considered and rejected: SDL3 forever.** An abstraction with exactly one implementation is a
guess. The roadmap makes this argument for seeding Metal at M7; it applies identically here, which
is why the native backend is a scheduled M11 task rather than a hope. The two decisions are recorded
together in the platform ladder.

**What SDL3 costs.** Some `DisplayServer` feature queries degrade to what SDL3 exposes —
transparency, tray items, IME positioning. `has_feature()` already exists for exactly this, so the
cost is answered by the interface rather than absorbed silently.

## 5 — doctest over Catch2

**Decision.** doctest, with one `CY_TEST_CASE` wrapper so the framework is replaceable.

`testing-and-quality` budgets unit tests at under a millisecond each and expects thousands of them,
and requires the pre-commit unit set to complete in under a minute. doctest compiles roughly an
order of magnitude faster than Catch2. Compile time is a tax every contributor pays on every build,
and `build-system-and-platforms` has its own build-performance requirement — this is the same
argument applied to tests.

Catch2's richer matchers and BDD sections map attractively onto the specifications' `#### Scenario`
blocks. That is a real loss, and it is smaller than the compile-time gain at the scale the taxonomy
implies.

## 6 — The dependency manifest is data, and attribution is generated

`thirdparty-dependencies` requires a manifest, pinned versions, vendoring policy and attribution.

**Decision.** `deps/manifest.toml` is the single source: name, version, exact commit, licence,
source URL, the engine-owned interface it sits behind, and the feature option that gates it. CMake
reads it to drive `FetchContent`; `tools/deps/` generates `THIRD_PARTY.md` from it; CI fails if the
generated file is not current.

**Why not vendor now.** Vendoring is a policy the specification permits, not one it requires. Pinned
commits give reproducibility without a tree full of other people's source at the milestone where the
dependency count is five.

## 7 — Four profiles, one meaning — spiked first

This is M0's named risk. `developer-workflow-and-just` requires that `debug`, `dev`, `profile` and
`release` mean the same thing in CMake, Cargo, the shader toolchain and the engine's own tools. Only
CMake exists at M0, and the failure mode is defining the profiles against the one toolchain present
and discovering at M5, when Cargo arrives, that the mapping does not survive contact.

**Decision.** The mapping is written down as data — `cmake/profiles.cmake` plus a table in the
`justfile` — before the recipe surface is built, with the Cargo and Slang columns filled in and
unused. The spike proves one profile end to end and confirms the empty columns are expressible.

| Profile | CMake config | Assertions | Editor | Cargo (M5) | Slang (M3) |
|---|---|---|---|---|---|
| `debug` | `Debug` | on | on | `dev` | `-O0 -g` |
| `dev` | `Development` | on | on | `dev` + opt-level 2 | `-O1 -g` |
| `profile` | `Profile` | off | off | `release` + debug | `-O2 -g` |
| `release` | `Shipping` | off | off | `release` + LTO | `-O3` |

## 8 — Feature options exist before their features do

`build-system-and-platforms` lists twenty-one `CY_*` options for subsystems that will not exist for
several milestones.

**Decision.** Declare the full set at M0, defaulting to `OFF` where the subsystem does not exist,
and generate `cy_features.h` and `cy_modules.h` from them. Each option's dependency declarations
(`CY_AI` requires `CY_NAVIGATION`) are recorded now, since they are specified and the validation
logic is written once.

The alternative — adding options as subsystems arrive — means the generated-header machinery, the
dependency validation and the "disabling excludes sources rather than stubbing at runtime" rule each
get retrofitted, and the last is the one that quietly does not happen.

## 10 — One recipe namespace, flat names, imported files

`developer-workflow-and-just` requires that bare `just` lists **every** recipe with its description,
and that names are predictable and consistent across categories.

**Decision.** The root `justfile` `import`s one file per category from `just/` —
`just/build.just`, `just/test.just`, `just/quality.just`, and so on — and recipes are named
`<category>-<verb>`: `env-doctor`, `build-engine`, `test-unit`, `quality-layers`,
`roadmap-status`, `roadmap-milestone`, `diagnose-trace`, `run-sample`.

**Considered and rejected: `just` modules and `category::recipe` names.** Modules are the obvious
fit for the category structure, but two things rule them out. `just` 1.21 — the version in use —
treats `mod` as unstable, and the project's central entry point should not depend on an unstable
feature. More importantly, with modules, bare `just` lists the *modules*, not the recipes, which
directly contradicts the discoverability requirement. Flat names in imported files give the file
organisation without losing the listing.

Import also gives every category file a single owner, which is what makes the workflow parallel.

## 9 — What M0 deliberately does not do

- **No containers, allocators, math or job system.** All M1. The M0 sample needs none of them, and
  writing them here means writing them before `core-type-system` exists to be written against.
- **No reflection.** M1, and the whole of M1's critical path.
- **No ECS, no scene, no serialization.** M2.
- **No renderer.** `DisplayServer` creates a surface; nothing consumes it until M3. The empty sample
  clears nothing — it opens a window.
- **No Rust, no Swift.** M5 and M4. The profile table reserves their columns; the toolchains are not
  installed or checked by `doctor` yet.
- **No editor directory.** M5. The layer table reserves the slot.
