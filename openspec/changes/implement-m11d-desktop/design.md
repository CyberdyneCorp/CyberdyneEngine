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

## 2. The interface is the milestone; the backends are its consequence

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

## 3. What counts as a delivered backend, and what is reported instead

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
