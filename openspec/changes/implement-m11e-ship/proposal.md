# M11.e — Ship: mobile, distribution, and the 1.0 record

## Why

**This project cannot produce a release, and it says so four times in its own recipes.**
`just/release.just` carries `release-version`, `release-changelog`, `release-artefacts` and
`release-publish`, and **every one of the four is a `_not-implemented` stub** that refuses and names
*"M12 — build-and-packaging"* as the milestone it waits for. **There is no M12.**
`record.MILESTONES` is fifteen entries ending at `m11`, and one of M11's exit criteria is *"version,
changelog and artefacts are produced by the release recipes"*. The same stale labelling was found and
corrected once already, in `just/content.just`: *"`build-and-packaging` reached Working at M6, not
M12: the roadmap moved it and these two recipes"*.

**What else is verified absent on the tree this rung starts from:**

- **There is no mobile toolchain.** `cmake/modules.cmake` declares the vocabulary —
  `set(CY_MODULE_PLATFORMS linux windows macos ios android visionos web)` — and then states the
  truth in its own diagnostic: *"Supported: Linux, Windows, Darwin. Planned: iOS, Android, visionOS,
  Web."* No toolchain file, no NDK or SDK integration, no cross-compilation target and no mobile leg
  exists anywhere in the tree. The only `android` strings in the repository are four Rust crates in
  `deps/rust-crates.toml`, each marked *"Not chosen"* and present because the editor's `eframe` and
  `wgpu` graph reaches them.
- **`build-system-and-platforms` is recorded at Seed**, appears in no milestone ledger but `m0.toml`,
  and what Complete waits on is named: *"distribution artefacts and cross-compilation to a second
  platform"*. Neither exists.
- **`thirdparty-dependencies` is recorded at Seed** with seventeen dependencies under full manifest
  governance, and Complete waiting on two things: *"about half the intended set is not integrated and
  the runtime exposes no attribution API"*.
- **`rendering-forward-clustered` has never seen a mobile pipeline.** It is recorded at Working from
  M3; M11.d takes its desktop half — MSAA and multi-view — and the mobile pipeline differences are
  the half nobody has written, on hardware nobody in this project has.
- **`xr-support` is the only capability recorded at `none`**, deferred by decision.
  `docs/roadmap/risks.md` names its prerequisites — *"multi-view rendering, runtime-driven frame
  timing, late latching — each a check from M3"* — and its re-entry as *"after 1.0"*. The deferral is
  a decision to keep, not one to revisit here; what this rung owes it is that the prerequisite checks
  still pass on the day 1.0 is recorded.
- **The full CI matrix is three independent legs, not a matrix.** `ci.yml` runs `linux-arm64`,
  `macos-arm64` and `windows-arm64` alongside their x86-64 siblings and nothing compares or combines
  them; M11.a builds the first job that does, for one digest comparison, which is not the same as a
  release matrix.
- **And the record itself is not finished.** `delivery-roadmap` is recorded at Working with
  twenty-two requirements, and the milestone whose exit is *"every capability is Complete or
  explicitly deferred"* is the milestone that has to make that sentence true **in the record**, not
  only in a document.

## What Changes

- **Mobile targets and cross-compilation** — a toolchain, a target, and an artefact for at least one
  mobile platform, with the porting surface M11.d proved against a stub platform now proved against a
  real non-desktop one.
- **`rendering-forward-clustered` to Complete** — the mobile pipeline differences, with M11.d's
  desktop half already in.
- **`build-system-and-platforms` to Complete** — cross-compilation, distribution artefacts and the
  full continuous-integration matrix.
- **`thirdparty-dependencies` to Complete** — the rest of the intended dependency set integrated
  under the manifest's governance, and the runtime attribution API.
- **Distribution** — the four release recipes stop refusing: version, changelog, artefacts and
  publish, produced from the recipes rather than by hand, plus downloadable content and distributed
  execution if M11.d has not already carried them.
- **Every remaining capability to Complete, or an explicitly recorded deferral with its re-entry
  point.** This rung is where the sweep happens, and it is the only rung allowed to record a
  deferral: any row an earlier rung demoted arrives here **with the reason that demoted it**, and
  leaves either Complete or deferred with a named re-entry, never silently.
- **`xr-support` stays deferred with its prerequisites checked**, as a decision restated rather than
  a row skipped.
- **The editor's material authoring front end, moved here from M11.c and beside the `.cygraph`
  writing it depends on.** `editor/crates/cy-editor-interface/src/specialised/graph.rs` assigned
  *writing `.cygraph` from Rust* to this rung before M11.c started, giving the reason — *"a second
  writer of a canonical format is a second format the day the two disagree about a float"*. M11.c's
  spike then found junction 1 of its authoring path REFUSING, and behind the refusal were three
  pieces in two languages rather than one task. **M11.c built two of them** — the engine's
  `lower_material` and its 25 node types, and `Domain::Materials`' vocabulary and pins in Rust, so
  the material editor opens and its nodes wire — **and the third was already this rung's**: a front
  end a person drives is a front end that saves, and what it saves is the canonical format Rust is
  not allowed to write. What arrives here is the `material.*` command registry
  `cy_editor_services` does not have, the front end that drives it, and the stage comparison M11.c
  task 1.3 could not make without one. M11.c design.md §1.3c records the move; tasks section 5a
  carries it.
- **`delivery-roadmap` to Complete, and the 1.0 record written** — the matrix, the status record and
  the ledgers agreeing, and the statement of what 1.0 is and is not.

## Capabilities

**Four rows to Complete — two from Working and two from Seed** — carrying **55 requirements**:
`build-system-and-platforms` and `thirdparty-dependencies` (Seed, M0), `delivery-roadmap` (Working,
M0) and `rendering-forward-clustered` (Working, M3). **All four were last advanced at M0 or M3**,
which makes this the rung with the oldest tiers on the ladder and the smallest row count.

**Plus the sweep, whose size this rung cannot know at proposal time.** M11 owes 65 Complete cells
across five rungs; M11.a names `save-and-persistence` and `audio` as its likely demotions, M11.b
names `ml-inference` and `swift-scripting`, M11.c names `rendering-culling-and-lod` and M11.d names
`build-and-packaging`. **Six named candidates and an unknown number of unnamed ones arrive here**, and
the honest position is that this rung's real load is whatever the four rungs above it could not
finish.

**And `xr-support`, the one row of 76 that reaches 1.0 without a Complete cell**, deferred with its
prerequisites verified.

## What is contingent, and what this rung predicts about itself

- **This rung is deliberately last, and deliberately the one that may be smallest in rows and largest
  in work.** Mobile is not more difficult than the other four rungs; it is the one whose failure
  costs least, because a desktop engine with an editor, a game, a beauty shot and a package is a real
  engine that does not run on phones, while a mobile engine with none of those is nothing.
- **No hardware here can judge it.** This host is Linux on x86-64 with one GPU vendor; there is no
  Apple toolchain, no Android device and no mobile runner in `ci.yml`. Every mobile claim will be
  *authored and CI-judged*, and where CI cannot judge it the criterion must report **NOT EVALUATED**
  rather than pass — the mechanism exists (`where = "ci"`), it has been used twice before, and an
  unevaluable criterion reported as green is the one outcome worse than a red one.
- **The sweep is where a milestone lies to itself**, and this rung's job is to make that hard. A
  deferral is only honest with three things: what is unmet, why it is deferred, and the condition
  that brings it back. A row arriving here with none of those is a row that has to be finished, not
  recorded.
- **One inherited task succeeds by turning another rung's green criterion red, and that is the
  criterion working.** `m11c:the-shot-does-not-overclaim-the-editor` asks the editor for its command
  registry and fails the day a `material.*` command appears in it, because M11.c's beauty shot was
  authored through the tooling that exists rather than through a material graph editor and its
  caption says so. Landing task 5a.1 owes `docs/design/beauty-shot.md` an updated caption in the same
  change. **The caption is what changes; the criterion is not weakened to keep it green.**
- **`thirdparty-dependencies` at Complete requires integrating *"about half the intended set"*** —
  a phrase this rung will have to turn into a list before it can be scoped, and the list may well be
  shorter than the intent, in which case the intent is what changes, through a change against the
  specification.
- **The prediction this rung makes about itself**: if any rung on this ladder closes with rows still
  open, it is this one, and the correct outcome is **1.0 recorded with a named deferral list** rather
  than 1.0 delayed until a phone works. `delivery-roadmap`'s own exit says *"Complete or explicitly
  deferred"*, and the second half of that sentence is not a loophole — it is the mechanism.

## Impact

- **New code**: mobile toolchain files and targets, the mobile pipeline path in the forward renderer,
  the release recipes' implementations, distribution artefact assembly, the runtime attribution API,
  the editor's `material.*` commands and the material graph front end inherited from M11.c, and
  whatever the sweep turns out to carry.
- **Existing code**: `just/release.just`'s four stubs and their reference to a milestone that does
  not exist; `cmake/modules.cmake`'s "Planned" list becomes shorter by at least one entry.
- **Machinery**: the full CI matrix; the final ledger, whose `expect_tiers` is the 1.0 claim itself;
  the status record and the capability matrix brought into agreement with it **at the closing gate,
  which is where a tier is recorded and nowhere else**.
- **Closing artefact**: `samples/11-ship` on every supported target from a single recipe — the
  desktop half from M11.d and at least one mobile target here — and **the 1.0 record**: 76 rows, each
  Complete or deferred with a re-entry point, with every deferral naming what is unmet and what would
  bring it back.
- **Risk**, and the rung's named spike: **find out whether a mobile artefact can be produced in CI at
  all, before anything else in this rung is scoped.** One empty project, cross-compiled, packaged and
  reported from a hosted runner. If that cannot be done, the mobile scope is a deferral with a
  re-entry point and this rung is the distribution and record rung — which is a finding worth having
  on day one rather than at the gate.
