# Design: M11.d — Desktop

## 1. The spike, and the two questions it has to answer before anything is scoped

`docs/roadmap/risks.md` names one spike per milestone and this rung's is stated in its proposal:
**settle the eight RHI gaps as interface changes, on Vulkan and null, before a line of either backend
is written.** The seed exists to make that finding cheap and spending it is the first task. A second
question rides with it because it costs one CI run and decides how every image claim in this rung is
*reported*.

### 1.1 The interface question

`src/backends/rhi-metal/` is a seed that has never been compiled — its own README's first sentence
— and what it carries instead of a backend is a finding: **eight gaps, three where Metal has no
equivalent at all, two with no workaround.** `metal_gaps()` returns them as data so the README, the
diagnostic and `unit.rhi_metal_seed` cannot drift apart.

What the seed does **not** carry is a cost. Each gap has a proposed fix and none of them has been
applied to anything. The spike applies all eight to `VulkanDevice`, `NullDevice` and
`src/rendering/graph/`, and counts what moves. The call sites are already known:

| Gap | Interface | Consumer | Workaround |
|---|---|---|---|
| 2 — transient memory type bits | `device.h:254` | `executor.cpp:192` | **none** |
| 5 — secondary recorded before its pass | `device.h:336` | `executor.cpp:417` | **none** |
| 4 — queue families | `device.h:182` | barrier and ownership paths | yes |
| 6 — pipeline cache as a blob | `device.h:305` | cache persistence | yes |
| 1, 3, 7 | shader module, image layout, format | backends and the graph | yes |
| 8 — multi-stage push constants | — | — | genuinely fine |

**The answer the spike must produce is a number, not an opinion**: how many call sites each change
moves, and whether the two with no workaround can be made on the two backends that exist without
touching a pass. A change that cannot be made cleanly on two backends will not be made cleanly on
four.

### 1.2 The runner question, which is not a code question

Every Metal and D3D12 claim this rung makes is judged on a machine nobody here owns. `ci.yml` runs
six legs — `linux-x86_64`, `linux-arm64`, `macos-x86_64`, `macos-arm64`, `windows-x86_64`,
`windows-arm64` — that build and test, and **not one of them has ever created a graphics device.**
The `render` job says so in its own comment block: the default build has `CY_RENDERER_VULKAN` off,
what `just test-render` runs there is `render.null_frame` and `render.xr_prerequisites`, and the
golden images are deliberately not in CI at all because *"a committed reference is a photograph of
one implementation"*.

So the spike probes: does `macos-14` present a `MTLDevice` and a `CAMetalLayer`, and does a Windows
runner present a D3D12 device — and **what kind**. There are three honest outcomes and the rung is
shaped differently by each:

| Outcome | What this rung then claims |
|---|---|
| A hardware device on a leg | the image criteria are ordinary tests, and this rung's central risk evaporates |
| A software or paravirtual device (WARP, a virtualised Metal device) | the criteria run, and the **result names the device**; parity against a reference photographed on hardware is reported as a measured delta, not as a tick |
| No device | a compile-and-validate claim, with the image criterion **reported NOT EVALUATED** through `where = "ci"`, exactly as `m0:three-platforms` is reported today |

### 1.3 What the spike cannot answer here, and how that is said

Through the ledger's own mechanism rather than as a sentence in a document. This host has one
operating system and one GPU vendor. `requires = "gpu"` would be *met* here — the machine has a DRM
render node — so the criterion would run and pass on evidence about one vendor's driver, which is
the exact defect `requires` exists to prevent. `where = "ci"` with a `reason` is the mechanism's way
of saying "another machine", and M10's ledger header already wrote that rule down.

> **The spike measured this paragraph and half of it is false.** This host enumerates **three Vulkan
> devices from two vendors** plus a software rasteriser — NVIDIA GeForce RTX 5060, Intel UHD
> Graphics 770 and llvmpipe — and they answer the gap-2 question *differently* (§1.4). So
> `requires = "gpu"` here is not one vendor's driver; it is three answers, which is better evidence
> than this paragraph assumed. The conclusion survives with a different reason: **what needs
> `where = "ci"` is the operating system, not the GPU vendor.** Metal and D3D12 are unreachable here
> because this is Linux, and no count of local devices changes that.

### 1.4 THE SPIKE'S ANSWER — run before any of section 1

Run 2026-09-19, before any of section 1. Recorded here so the rows that depend on it read it rather
than re-derive it. The spike itself is `~/cyberdyne-spikes/m11d-desktop-spike/` — outside the
repository, as every spike since M3 has been — and `RESULT.txt` there carries the numbers in full.

#### 1.4.1 The runner question: **a device on every leg that ran, and not one of them is a GPU**

**Method, stated because it decides how strong this is**: a throwaway GitHub Actions workflow on
`spike/m11d-device-probe`, whose tree is *only* the workflow and two probe programs, so pushing it
ran this and none of `ci.yml`. Each probe **creates a device, clears a 64×64 target to a known
colour, reads the pixel back, and presents.** That is the strong form: a frame that happened, not a
runner-image manifest saying an SDK is installed. Reproduced identically across two runs
(`35438833469`, `35439163646`).

| Leg | Device | Drew | Presented | What kind |
|---|---|---|---|---|
| `macos-14` (arm64) | `MTLDevice` **yes** | yes, pixel exact | `CAMetalLayer` headless, `presentDrawable` clean | **"Apple Paravirtual device"** |
| `macos-13` (x86_64) | — | — | — | **no runner was ever allocated**, across both runs |
| `windows-2022` (x64) | `ID3D12Device` **yes** | yes, pixel exact | swapchain on a hidden HWND, `hr=0` | **"Microsoft Basic Render Driver"** — software |
| `windows-11-arm` | `ID3D12Device` **yes** | yes, pixel exact | `hr=0` | same, software |

**Neither leg is outcome A.** Both are §1.2's outcome B — *a device that must be labelled what it
is* — and the labelling matters more than this rung expected, because the runners are missing the
specific features its own acceptance text names:

- **The macOS device reports no Apple GPU family at all** (`Apple7=0`, `Apple8=0`, `Metal3=0`,
  `Mac2=1`). **Tile memory and memoryless attachments are Apple-family features**, and they are §7's
  entire stated reason for refusing MoltenVK. *The thing a native Metal backend is for cannot be
  exercised on any hosted runner.*
- **Argument buffers are Tier 1** on that device. The engine's descriptor model is bindless; the
  bindless path needs Tier 2. So `integration.rhi_metal` can pass there without the descriptor model ever
  having been exercised.
- **There is no hardware GPU on any hosted Windows image**, and the trap is sharper than "it is
  WARP": the probe found **two** adapters, both `Microsoft Basic Render Driver`, and **adapter 0 does
  not set `DXGI_ADAPTER_FLAG_SOFTWARE`**. The ordinary "pick the first adapter without the software
  flag" selects a software rasteriser. A D3D12 backend that trusts that flag will report hardware it
  does not have, and so will its golden images. Task 3.5's "a WARP adapter is labelled WARP in the
  result" is therefore not enough on its own: **the label has to come from the adapter's identity,
  not from its flag.**
- `isDepth24Stencil8PixelFormatSupported = 0` on the Metal device, and `ResourceHeapTier = 2` on the
  Windows device — the first confirms gap 7's Metal claim first-hand, the second is a *hosted*
  answer to a question only *hardware* can answer, since Tier 1 hardware still exists.

#### 1.4.2 The interface question: the counts, and three places the seed's own proposal is wrong

Occurrences over `src/` and `tests/`, classified by the area an edit lands in, the seed's own files
excluded because changing them is the point.

| Gap | Sites that move | What the measurement says |
|---|---|---|
| **2** — transient memory | **20**, and **9 files / +46−18 when actually applied** | **prototyped, built and run.** Cheapest of the eight, and it is one of the two with no workaround |
| **5** — secondaries | **1** | the count is meaningless; the mismatch is structural, below |
| **3** — `ImageLayout` | **~112**, of which **47 are tests** | **the expensive one by an order of magnitude**, and the seed files it as "workaround: yes" |
| **4** — queue families | **1** | exactly **one** call to `device->queue_family()` outside tests, in `executor.cpp`. The 51 `src_`/`dst_queue_family` *field* uses stay either way |
| **1** — shader form | **0** | purely additive; 9 renderer and 9 test sites keep passing `spirv` untouched |
| **6** — pipeline cache | **0** | **nothing in the tree calls `save_pipeline_cache` or `load_pipeline_cache`** — not one caller, tests included |
| **7** — `D24UnormS8Uint` | **0** | **the per-format query already exists and is already populated by both backends** |
| **8** — push constants | **0** | confirmed: genuinely nothing |

**Gap 2 is settled, and the seed's fix is wrong in one word.** The README proposes an opaque
`MemoryPoolClass` *"the graph only compares for **equality**"*. Measured on this host's three
devices: NVIDIA answers `0x03` for transient images and `0x1F` for transient buffers — **they
differ** — while Intel answers `0x07` and llvmpipe `0x01` for everything. An equality would refuse
to put images and buffers in one pool on the NVIDIA device and **split the transient heap in two**,
which is a direct loss of the aliasing `heap_bytes` vs `naive_bytes` exists to report. **A meet
(`a & b`, empty when zero) keeps the proof, loses the Vulkan spelling, and costs nothing extra** —
and it needs no device, so `compile()`'s "the derivation touches no device" invariant and
`plan_hash`'s determinism both survive. D3D12 wants the same shape: on **Resource Heap Tier 1** a
heap holds buffers *or* textures and never a mix, which is the same partition Vulkan spells as a
bitmask.

What "an afternoon" turned out to be: **9 files, +46/−18 lines of which 22 are the doc comment, 18
objects rebuilt, zero test sources changed, zero passes changed, zero files under `src/rendering/`
outside the graph** — and then `unit.render_graph` 29/29, `unit.rhi` 29/29,
`integration.render_graph_scale` 13/13 and `smoke.vulkan_frame` 4/4 **on the real RTX 5060**,
including *"transient aliasing reduces the device's own reported heap usage"*, which is the case
this change could have broken. The patch is `out/gap2-prototype.patch`.

**Gap 5's proposed fix does not address the actual mismatch, and the real fix is cheaper.** The
README proposes stating *"the pass is begun before its secondaries are recorded"* as a precondition.
Read against the tree that is not the shape of the problem:

- `executor.cpp:337-382` records **every** secondary for a submit, one per pass, on job workers,
  **before the primary loop reaches any of them**;
- `frame_recorder.cpp:209,232,254,287` — the pass callback itself calls
  `begin_rendering`/`end_rendering`, so **each secondary contains a whole render pass**;
- `executor.cpp:385-425` records the barriers into the **primary**, between passes.

`MTLParallelRenderCommandEncoder` is parallelism **within** one render pass. This engine's is
parallelism **across** passes. They are different axes and no ordering precondition converts one
into the other; Metal's actual equivalent is one `MTLCommandBuffer` per pass with `-enqueue`
establishing order, which is a different allocation strategy rather than a reordering. And
`ExecuteOptions::parallel_recording` **defaults to `false`** (`executor.h:66`), so the feature is
opt-in and off. **The honest fix is a capability — one term at `executor.cpp:334` — not a
precondition the null backend polices.** Task 1.2 should be rewritten to that before it is worked.

**Gaps 6 and 7 are not interface work; they are M8.c's firewall finding in a third module.**
`capabilities.h:74-96` already defines `FormatFeature`, `DeviceCapabilities::format_features()`
already answers per format, and `vulkan_instance.cpp:616-642` and `null_device.cpp:186-194` already
populate it for **every** format — and **nothing outside `src/backends/rhi/` calls it.** Every depth
path in the engine defaults to `D32Sfloat`, which Metal supports, so gap 7 is inert today. The
pipeline cache is the same shape and worse: two interface methods, four implementations, **zero
callers**, and a requirement — *"the cache is persisted across runs, so a warm start compiles
nothing"* — that is therefore unimplemented above the RHI. **Noticing these is worth more than the
signature changes**, and neither belongs in a list called "the eight gaps blocking Metal".

#### 1.4.3 What the answer does to the shape of the next two rungs

The interface work of section 1 is **cheaper than this rung assumed and differently distributed**:
gap 2 is settled, gaps 1, 6, 7 and 8 move nothing, gap 4 moves one call, and **gap 3 is where all
the cost actually is** — the one the seed files as workaroundable. Section 1 should be re-ordered to
match the measurement rather than the table.

The backend work of sections 2 and 3 is **verifiable on a hosted runner only in a form that stops
short of what its criteria claim**: compile, validate, create a device, draw, present, read a pixel
back — all real, all reportable — but **no Apple-family feature, no argument-buffer Tier 2, no
hardware of any vendor, and therefore no golden-image parity against references photographed on this
project's own hardware.** That is a *labelled software-and-paravirtual* claim plus a set of declared
deferrals, and it is what the rung those sections move to has to be written against.

**Where sections 2 and 3 go, and why.** They are **not deleted and not descoped** — they are moved
out of M11.d into **a rung of their own inserted between M11.d and M11.e**, and
`m11d:golden-images-across-three-backends` moves with them so that rung cannot close on "it compiles
somewhere". The reason is the machine, not the plan: **Metal cannot be compiled on Linux and there is
no Apple toolchain here, and D3D12 cannot be compiled on Linux either**, so neither backend could be
written or judged where this rung is being worked. Half-building them here would produce exactly the
defect this project has paid for nine times — a check that cannot fail. What stays in M11.d is
everything this host can actually check: the interface (§1, and 1.4.2 says what that now costs), a
**second native platform backend on Linux** — which is what proves the abstraction carries no SDL
assumption and needs no Apple hardware — the core rows the port audits, the four absent quality
gates, and `samples/11-ship` packaged and drawing. **1.4.1 is the evidence the moved rung is
written against**, and it says that rung is *partly* verifiable: real devices that draw and present,
labelled paravirtual and software, with the Apple-family features, argument-buffer Tier 2, hardware
parity and Resource Heap Tier 1 as **declared deferrals** rather than criteria a hosted leg can
answer. Those four are written down with their re-entry points in
[`implement-m11d5-backends`](../implement-m11d5-backends/design.md) §2, **before** that rung's first
line of backend code, because a deferral declared after a gate has looked at it is an excuse and one
declared before it is scope. `rhi-and-render-graph`'s Complete cell moved with them; the governing
rule is `delivery-roadmap`'s "A spike may resize a milestone as well as redirect it", which requires
re-scoping to move whole capabilities with their exit criteria rather than narrowing one in place.

## 2. The interface is the milestone; the backends are its consequence — now literally, a rung above

**§1.4.3 turned this section's argument into the shape of the ladder.** What follows was written as
two rules a task list had to be trusted to honour; both are now properties of the rungs, because the
backends are M11.d.5's and this rung has no Metal and no D3D12 to fix a gap inside. Rules 1 and 2
still govern — they are what M11.d.5 will be judged against when it writes those backends — and this
rung is where they are paid for.

The named risk of this rung is **not** that Metal or D3D12 is hard. It is that
`reserve_transient_memory`'s contract is Vulkan spelled into an engine-owned interface, and the cost
of finding that out late is a migration across every pass. Two rules follow and they order the whole
of sections 1 to 3 of `tasks.md`:

1. **No gap is fixed in a backend.** Every one of the eight lands in the interface and on the two
   backends that exist first. A fix that appears in the Metal backend is a fix the Vulkan backend
   never agreed to.
2. **Every fix is a capability query, never a backend identity test.** `Backend capability model`
   already requires the renderer to branch on capabilities and never on which backend is loaded, and
   the eight gaps are the first real pressure that rule has been under. `needs_queue_ownership_transfer()`
   and the per-format support query are that rule applied; `if (backend == Metal)` anywhere above
   `src/backends/` is this rung failing.

There is a third rule with a sharper edge, and it is why gap 2 is task 1.1 rather than task 1.8:
`memory_type_bits` is **intersected across every transient in a frame to prove one pool is legal for
all of them.** That proof is load-bearing; the Vulkan spelling of it is not. An opaque pool class the
graph compares for equality keeps the proof and loses the spelling — and a backend that can only
answer `~0u` stops having to hope nobody looked.

## 3. What counts as a delivered backend, and what is reported instead — M11.d.5's section now

**Kept here because it was decided here and the decisions did not change when the rung did.**
`implement-m11d5-backends`'s `design.md` §3 carries the same three, and that is the copy a backend
author should read; this one is the record of where they came from.

`delivery-roadmap`'s M11 exit criterion is *"golden images match across Vulkan, Metal and D3D12
within tolerance"*. This rung has to be able to say that sentence honestly or say something else, and
three decisions make that possible:

- **Vulkan's references stay the references.** A second committed reference per backend would make
  every backend its own truth and no comparison would ever fail. Metal and D3D12 are compared against
  the images already in `tests/render/references/` with a perceptual tolerance, and the per-backend
  delta is **reported** rather than thresholded away. `Backend divergence` is already a scenario of
  `testing-and-quality`: the test fails identifying both backends, because parity is the requirement.
- **A device is named in the result.** Hardware, paravirtual, or WARP. A green tick that does not say
  which answered is a claim about hardware nobody ran, and this project has already paid twice for a
  criterion that passed on evidence it never examined.
- **NOT EVALUATED is never a pass, and it is also never a failure to be hidden.** A leg with no
  device produces a compile-and-validate claim and an image criterion that reports itself unevaluated
  with its reason. That is a worse outcome than a photograph and a much better one than a tick.

## 4. Which desktop gets the native backend, and the trade that choice makes

`core-platform-abstraction`'s Complete cell needs **one** native `Platform` and `DisplayServer`
replacing SDL3, and the exit criterion is that `src/core/`, `src/ecs/`, `src/servers/` and
`src/scene/` do not change. Two candidates, and the choice is a trade rather than a preference:

| Candidate | For | Against |
|---|---|---|
| **Linux native** (Wayland, with X11 through xcb) | judged on the machine the work happens on, where a failure can be debugged rather than bisected through CI; the M3 golden images and `samples/00-empty` can both be run here | it is the platform SDL3 is best tested on, so it proves the abstraction against the *most* familiar windowing model |
| **macOS native** (Cocoa, `CAMetalLayer`) | Metal needs a surface anyway, so the two halves share work | judged only on a hosted runner nobody here can debug, on the same leg the Metal backend is being written blind against — two unproven things standing on each other |

**The plan is Linux native**, and the residual is recorded rather than argued away: a Linux native
backend proves the abstraction carries no SDL assumption; it does **not** prove the abstraction
against the platform whose windowing model is least like SDL's. The **stub platform** of task 4.4 is
what covers that gap deliberately — no mouse, no resizable window, no writable filesystem outside the
user mount, no ownership of the main loop — and it is the cheaper and stricter of the two proofs,
because it shares no desktop assumption at all rather than a different set of them.

**What would overturn this**: if 1.2 finds a real Metal device on a hosted runner and the Metal
backend reaches the golden images early, Cocoa becomes the better second implementation and this
section is wrong in the useful direction. The decision is written here so that reversing it is a
recorded change rather than a drift.

## 5. Which of these rows are actually contingent, stated rather than assumed

Ten rows to Complete, seven of them last advanced at M0, M1 or M2, is the oldest set of tiers in the
record — and they are not equally risky.

- **`rhi-and-render-graph` is the rung.** Two backends and the interface they force. If section 1's
  interface work goes badly, nothing else here is worth judging, because `rendering-forward-clustered`
  and every image claim run through it.
- **`core-platform-abstraction` is bounded and its criterion is a diff**, which is unusually good
  news: the claim is checkable mechanically and cheaply, and a failure is informative rather than
  vague. §4's residual is the only soft part.
- **`testing-and-quality` is contingent on M11.a in a way no other row here is.** Its Working tier is
  one of the four that **no criterion in any of the fifteen ledgers evaluates** — the only
  `expect_tiers` entry naming it is `m0:roadmap-tiers` expecting `seed`, and an exit tier is a floor,
  so nothing can contradict a Working claim. A Complete cell written on top of an unchecked Working
  tier is the defect M10's gate refused twice. **If M11.a has not produced a criterion that evaluates
  this row, this rung produces one before it claims the tier**, and §6 carries that as a dependency
  rather than an assumption.
- **`ecs-core`, `engine-architecture` and `core-jobs-and-concurrency` carry no named blocker**, which
  means nothing has refused them rather than that nothing is missing. The port is the audit. Task 6.3
  reads them requirement by requirement the way M10 read `save-and-persistence` — and that audit
  found nine satisfied, three unmet and eight partial on a row two milestones had called nearly done.
  **A similar table here would be a finding, not a failure.**
- **`core-assets-and-io` and `core-memory-and-containers` each have exactly one named absence** — a
  transport behind `RemoteFileProvider`, and a producer for the attribution axes — and both are small
  pieces of work whose value is that the mechanism stops being unexercised. `MemoryAttributionScope`
  is named by four files, all its own; that is M8.c's firewall finding in a different module, and it
  is the shape this project now recognises.

### 5.1 The demotion this rung predicts about itself

**`build-and-packaging`.** Its scope includes downloadable content and distributed execution, and
`docs/roadmap/risks.md` already lists distributed build execution as *"M11 or later"* — written
before the split existed, so "later" now has a name. Content audit and provenance are ordinary
engineering against a derivation graph that already exists; DLC is a mounting and signing story and
distribution is a second machine. **If the distribution surface only becomes real at M11.e, this
row's Complete cell moves there with its reason recorded**, and that is a demotion done properly
rather than a cell claimed thinly.

> **DECIDED — task 7.6, and the prediction held.** `build-and-packaging`'s Complete cell **moves to
> M11.e**. What this rung made real: the content audit per file, with the chain from a *declared*
> root and unreferenced content flagged (7.4); provenance with all seven fields and stripped
> binaries whose separated symbols are *proved* to match them, archived by build identity, plus a
> reproducibility bundle keyed by that identity (7.5). What did **not** become real here, each for a
> reason a code change in this rung could not remove:
>
> - **Downloadable content** is *"a signed package set with a declared dependency on a base
>   build"*. Nothing in `core/crypto` signs anything, and `tools/build/README.md` already declines
>   to invent a signature scheme because the requirement forbids it. Mounting by stable identity
>   exists; the signed set does not.
> - **Distributed execution** needs remote workers — a second machine. `BuildConfig::distributed`
>   is read and reported and has no worker pool behind it; the local degradation the requirement
>   asks for is the state the build ships in, and that is all a one-host rung can show.
> - **The reproducibility bundle archived *by CI***. The bundle exists and verifies on its own
>   (`samples/11-ship`, act 1c); archiving it is an upload step in `ci.yml`, which the close phase
>   owns and M11.e's full CI matrix rewrites.
> - **Size by plugin and by world region** stay `NOT REPORTED`: neither is a declaration
>   `cybuild 1` carries.
>
> The cell is received by M11.e task 4.4 and its criterion
> `m11e:downloadable-content-and-distributed-execution`, which already existed for exactly this;
> `m11e.toml`'s `roadmap-tiers` now expects the row there and `m11d.toml`'s no longer does.
> `m11d:build-and-packaging-moves-to-m11e` fails if either half of that move is undone.

**And a second, smaller one: `developer-workflow-and-just`.** Target selection and the second-platform
recipes are this rung's. The four `release-*` recipes are not — they refuse today naming *"M12 —
build-and-packaging"*, a milestone that does not exist on a ladder whose `record.MILESTONES` ends at
`m11`, and M11.e owns implementing them. The M11 exit criteria include *"version, changelog and
artefacts are produced by the release recipes"*. **This rung therefore cannot honestly write Complete
for this row unless M11.e's release work has already landed**, and task 7.8 exists to stop its own
gate from doing so.

M10's design predicted a demotion for `procedural-content-generation` and **was wrong, in the useful
direction, because a spike measured it.** That is the standard here. The prediction most likely to be
overturned the same way is §1.2's: if both hosted runners present devices, the image claims become
ordinary tests, `build-and-packaging`'s distribution half is the only soft row left, and this rung is
smaller than its proposal says. The prediction least likely to be overturned is gap 2 — it has no
workaround and no equivalent, and no measurement can make that untrue.

## 6. Dependencies on other rungs, named rather than absorbed

| Needed from | What | If it has not landed |
|---|---|---|
| **M11.c** | `shader-system` emitting **MSL and DXIL**. `cache.h` names `"metal-msl"` and `"d3d12-dxil"` as interchange forms, `compiler.h` describes the translation in a comment, and **nothing in the tree emits either**; `SLANG_ENABLE_DXIL` is OFF with "D3D12 turns this on" beside it | this rung writes the minimum target support its two backends need and **says so in its records**, and the row's Complete cell stays M11.c's |
| **M11.a** | criteria that **evaluate** `testing-and-quality`'s and `developer-workflow-and-just`'s Working tier | this rung writes them before claiming either Complete cell — §5 |
| **M11.a** | the CI job that publishes one leg's digest and compares it against another's | this rung's cross-backend and cross-platform comparisons are the **same machinery**; building it twice is the waste to avoid, and whichever rung reaches it first owns it |
| **M11.e** | `build-system-and-platforms` (cross-compilation, the mobile targets, the full matrix), `thirdparty-dependencies` (DXC, and a D3D12 allocator if one is adopted), and the four release recipes | this rung claims neither row and records the dependency in its gate |
| **M11.b** | the editor's own surface on a second desktop. The editor is a separate process with its own graphics stack; the native `Platform`/`DisplayServer` does not change it | out of scope here either way, and §7 says so |

**Nothing in this list is absorbed.** A rung that quietly does another rung's row is a rung whose
gate cannot say what it is looking at, which is the reason M11 was split in the first place.

## 7. What this rung deliberately does not do

- **No Metal and no D3D12**, which is the change §1.4.3 made and the reason every bullet below about
  a backend now reads as a constraint on M11.d.5 rather than on this rung.
- **No MoltenVK.** `rhi-and-render-graph` refuses it as the long-term Apple strategy by name, so that
  a native Metal backend can use tile memory, memoryless attachments and MetalFX directly. A
  translation layer would produce a green golden image and satisfy nothing.
- **No mobile, no console, no XR.** iOS and Android are M11.e's; consoles are out of scope by
  specification; `xr-support` stays deferred with its prerequisite tests — which already exist as
  `render.xr_prerequisites` — still passing.
- **No second reference-image set per backend**, for the reason in §3.
- **No new render graph and no new pass set.** The backends execute the graph the renderer already
  produces; if a pass has to change to accommodate a backend, that is a finding about the interface
  and it belongs in section 1, not in section 2 or 3.
- **No deletion of the SDL3 backend.** It stays, it stays tested, and it stays the default on the
  platforms that do not get a native backend. The second implementation is the proof; a replacement
  everywhere would be a bigger change with a smaller claim.
