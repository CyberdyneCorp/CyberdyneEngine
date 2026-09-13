# M11.d — Desktop: the same project on a second and a third desktop

## Why

**Eleven milestones in, this engine runs on one graphics API, through one windowing library, on one
operating system.** `delivery-roadmap`'s M11 row is about exactly that and nothing else in M11 is;
the other four rungs are debt, authoring, image and distribution. This rung is the milestone as the
plan actually wrote it.

**What is verified absent on the tree this rung starts from**, rather than assumed:

- **There is no Direct3D 12 backend.** `src/backends/` holds `rhi` (with `null` and `vulkan`),
  `rhi-metal`, `audio-miniaudio`, `physics-jolt`, `shader` and `viewport`. A search of the tree for a
  file named for D3D12 returns nothing; every `d3d12` string in `src/` is an enumerator in a
  capability, format or display-server table describing an API the engine does not have.
- **The Metal backend is a seed that has never been compiled.** `src/backends/rhi-metal/README.md`
  states it in its own first sentence: *"`src/device.mm` has never been compiled, on this machine or
  anywhere else — there is no Apple toolchain here"*. What the seed does carry is the finding it was
  built for: **eight gaps, three where Metal has no equivalent at all, and two with no workaround** —
  `reserve_transient_memory`'s `memory_type_bits`, which is intersected across every transient in a
  frame to prove one pool is legal for all of them and which Metal can only answer `~0u` to; and
  `execute_secondary`, where a secondary recorded before its pass instance exists has no
  `MTLParallelRenderCommandEncoder` equivalent.
- **SDL3 is the only way this engine opens a window.** `platform/` holds `desktop-sdl3`, `headless`
  and `host`, and `deps/manifest.toml` records SDL3's interface as *"`cy::Platform` and
  `cy::DisplayServer`, implemented in `platform/desktop-sdl3/`"*. Nothing proves the abstraction
  carries no SDL assumption, because nothing has ever implemented it twice.
- **`core-assets-and-io` cannot serve a file to a second machine.** `AssetSystem::reload` returns
  `NotImplemented` for an asset served from a cooked package (`asset_system.cpp:1190`), and
  "Development file serving" has no transport at all: `RemoteFileProvider`'s only implementation in
  the tree is `FakeHost` in `tests/test_vfs.cpp`.
- **`core-memory-and-containers`' memory diagnostics have no producer.** Attribution by domain, type,
  thread, world cell and asset was built and nothing pushes one: the four files naming
  `MemoryAttributionScope` are its own header, source, test and README.
- **`testing-and-quality` is recorded at Seed and four of its gates do not exist** — `swift-format`,
  the licence-header check, the spelling check and the undocumented-symbol gate — and its three
  acceptance scenarios (strategy stress, control handover, headless server) are unwritten. The
  documentation gate the M11 exit criteria name is the undocumented-symbol gate, and it is one of the
  four.
- **`developer-workflow-and-just` is recorded at Seed** with every recipe category doing something
  since M6 — `env`, `build`, `run`, `test`, `quality`, `generate`, `content`, `diagnose`, `roadmap`,
  `maintenance`, `release` — and Complete waiting on *"the targets a release and a second platform
  bring"*, which is this rung.
- **`samples/11-ship` does not exist.** `samples/` holds fifteen entries and none of them is a
  packaged project.
- **And the four-row problem M11.a inherits touches two rows here.** `testing-and-quality` and
  `developer-workflow-and-just` are two of the four whose Working tier is claimed by a matrix column
  and evaluated by no criterion in any of the fifteen ledgers. M11.a owes them a criterion; this rung
  owes them a Complete cell, and **the second is not worth anything without the first**.

## What Changes

- **`rhi-and-render-graph` to Complete** — **Metal, native and not a translation layer**, and
  **D3D12 to parity with Vulkan**. The eight gaps the seed recorded are interface changes before they
  are backends: an opaque memory-pool class the graph only compares for equality, layouts derived
  from the access masks `access.h` already carries, a queue-ownership capability query, a
  per-format support query, a pipeline-cache token, and a precondition on secondary recording.
- **`core-platform-abstraction` to Complete** — a **native** `Platform` and `DisplayServer` for one
  desktop platform, replacing SDL3 there, **requiring no change in `src/core/`, `src/ecs/`,
  `src/servers/` or `src/scene/`**, and the porting surface built against a stub platform that shares
  no desktop assumption.
- **`rendering-forward-clustered`'s desktop half** — MSAA and multi-view. The row's Complete cell
  stays at M11.e with the mobile pipeline differences, because a row is not Complete on the half of
  its scope this rung can reach.
- **`build-and-packaging` and `testing-and-quality` to Complete** — content audit, provenance and
  symbols, the full gate set and the documentation gate.
- **`developer-workflow-and-just` to Complete** — the release targets and the second-platform
  targets, from the same recipes on every desktop.
- **The core rows finished where the port can prove them** — `core-assets-and-io`,
  `core-jobs-and-concurrency`, `core-memory-and-containers`, `ecs-core` and `engine-architecture`.
  These are here rather than in M11.a for one reason: **the exit criterion for the native backend is
  that none of `src/core/`, `src/ecs/`, `src/servers/` or `src/scene/` changes**, which is a
  first-hand audit of those rows whether or not anybody calls it one.
- **`samples/11-ship` on desktop** — one project built, cooked, packaged and launched on each
  desktop target from a single recipe.

## Capabilities

**Ten rows to Complete — eight from Working and two from Seed** — carrying **129 requirements**.

From **Seed**: `testing-and-quality` and `developer-workflow-and-just`, both recorded at M0.
From **Working**: `rhi-and-render-graph` (M3), `core-platform-abstraction` (M4),
`build-and-packaging` (M6), `core-assets-and-io`, `ecs-core`, `engine-architecture` (M2),
`core-jobs-and-concurrency` and `core-memory-and-containers` (M1).

Seven of these ten rows were last advanced at M0, M1 or M2 — two at M0, two at M1 and three at M2 —
so this rung carries the oldest tiers in the record, and is the first to read them at Complete grade.

## What is contingent, and what this rung predicts about itself

- **This host has one operating system and one GPU vendor**, and that is the rung's defining
  constraint rather than an inconvenience. Every Metal and D3D12 claim must be judged on a hosted
  runner, and **whether a hosted runner can present a device at all is the first thing this rung must
  measure, not assume.** `ci.yml` already carries `macos-arm64` and `windows-arm64` legs; what it has
  never done is run a graphics device on one. If a leg cannot present a device, the honest claim is a
  compile-and-validate claim with the golden-image criterion **reported as not evaluated**, exactly
  as `m0:three-platforms` is reported today — not a green tick over an unphotographed frame.
- **The named risk is the interface, not the backends.** Two of the eight gaps have no workaround,
  and both sit in interfaces the render graph depends on. Changing `reserve_transient_memory`'s
  contract after two backends are written is a migration across every pass in the engine; changing it
  before is an afternoon.
- **`build-and-packaging` is the row this rung predicts it may demote.** Its scope in the plan
  includes downloadable content and distributed execution, and `docs/roadmap/risks.md` already lists
  distributed build execution as *"M11 or later"*. If the distribution surface only becomes real at
  M11.e, this row's Complete cell moves there **with its reason recorded**, which is what a demotion
  is for.
- **`testing-and-quality` is contingent on M11.a in a way no other row here is.** Its Working tier is
  one of the four nothing evaluates. A Complete cell written on top of an unchecked Working tier is
  the defect M10's gate refused twice; if M11.a has not produced a criterion that evaluates the row,
  **this rung must produce one before it claims the tier.**
- **`ecs-core`, `engine-architecture` and `core-jobs-and-concurrency` carry no named blocker**, which
  means nothing has refused them rather than that nothing is missing. The port is the audit; if the
  port changes `src/ecs/` or `src/core/`, that change **is** the finding, and it is more valuable
  than the Complete cell.

## Impact

- **New code**: a D3D12 backend; a Metal backend behind the seed's mapping layer; a native `Platform`
  and `DisplayServer` for one desktop; a stub platform for the porting surface; a transport behind
  `RemoteFileProvider`; memory attribution producers; four quality gates and three acceptance
  scenarios; the desktop half of `samples/11-ship`.
- **Existing code**: the RHI interface changes the eight gaps name, applied across the render graph
  and both existing backends before either new backend is written.
- **Machinery**: CI legs that build and run with a device where a runner has one, and report NOT
  EVALUATED where it has not.
- **Closing artefact**: `samples/11-ship` built, cooked, packaged and launched on each desktop target
  from one recipe, plus the M0 sample and the M3 golden images on the native platform backend with
  `src/core/`, `src/ecs/`, `src/servers/` and `src/scene/` untouched.
- **Risk**, and the rung's named spike: **settle the eight RHI gaps as interface changes, on Vulkan
  and null, before a line of either backend is written.** The seed exists to make that finding cheap;
  spending it is this rung's first task.
