# M11 — Reach: the same project, everywhere, from one command

## Why

**M10 made the engine's world large; nothing yet makes it portable, and nothing yet finishes
anything.** The engine now fills a world — terrain, water, foliage, weather, an atmosphere, a
procedural generator that places it all and a substrate with one producer per field — captures it,
replays it bit-exactly, replicates it to four peers under loss, and saves it. What it cannot do is
run anywhere but this desktop, on this one graphics backend, and **sixty-five of its seventy-six
capabilities are still at Working or below.**

`delivery-roadmap` gives M11's exit as *"every capability is Complete or explicitly deferred"*, and
the capability matrix's own reading of that is that **sixty-five capabilities reach Complete at M11,
and that is the definition of 1.0**.

### What this milestone inherits, each as a running, failing, rung-bearing criterion

Not a list of intentions. Every one of these is in a ledger under `tools/roadmap/milestones/`, runs
on every evaluation, fails today, and names M11 as the rung that closes it. A declared gap that
starts passing **fails the ledger** until its declaration is deleted, so none of them can be quietly
fixed and none can be quietly forgotten.

| Gap | Declared at | What is actually wrong |
|---|---|---|
| `m10:sky-field-round-trip` | M10 | `CloudShadowField::update` reports writing tiles darker than 0.5 and `CloudShadowField::sample` returns the declared 1.0 at all twenty-five points inside `radius_metres`. Terrain and water round-trip through the same store, so it is the sky's write path. Nothing outside `src/rendering/sky/` reads the field either |
| `m10:fields-one-vegetation-potential` | M10 | `cy::foliage` declares `vegetation-potential` UNorm8/Static/Presentation-side and `cy::weather`'s ecosystem half declares it UNorm16/SlowlyVarying/Authoritative, so `FieldRegistry::declare()` refuses the second in either order and a project registering both producers **fails at startup** |
| `m10:fields-sampled-on-a-device` | M10 | **zero `.slang` modules sample an environment field.** `environment-fields`' CPU-and-GPU-access requirement is discharged on the processor only, and `cy/field.slang` — which `src/environment/`'s README says is owed by the first renderer-facing row to sample a field in a shader — is unwritten |
| `m10:world-frame-budget` | M10 | the environment demo costs about 122 ms a frame with a device drawing and about 106 ms headless against a 16.7 ms budget — **seven times over** — nearly flat across the day/night cycle. The three largest bands are the substrate re-sampled at every terrain vertex (63.0 ms), the cloud march (23.2 ms) and water's foam field (12.0 ms) |
| `m9:lockstep-cross-platform` | M9 | this host has one architecture and one operating system, and no CI job compares a state hash between two of them. The legs exist and run independently; what is missing is an upload-then-compare job |
| `m8c:steam-audio-configures` | M8.c | `-D CY_AUDIO_STEAM_AUDIO=ON` does not configure and `SteamAudioBackend::simulate` returns `NotImplemented`. M8.c measured the whole cost — four upstream dependencies and an ABI flag that blocks both pinned compilers |

**And two questions this host is not able to ask**, reported NOT EVALUATED through the ledger's own
`where = "ci"` mechanism rather than passed on evidence that does not support them:
`m10:pcg-regeneration-cross-platform` (does a generated region reproduce on a second architecture?)
and `m10:pcg-gpu-domain-agreement` (does the GPU execution domain reproduce the CPU domain's output
on more than one vendor's driver?). Both are the same shape as `lockstep-cross-platform` and all
three want the same thing: **a CI job that publishes one leg's digest and compares it with another's.**
That job does not exist, and building it once would answer three criteria rather than one.

### And one finding about the plan that M10's gate was told to hand here rather than absorb

**`save-and-persistence` is mis-scoped rather than late.** M9 demoted it and named two blockers; M10
demoted it a second time, and `design.md` §4 of M10 wrote down in advance what that means — a second
demotion of one row is a finding about the plan, and it belongs in the next milestone's proposal.

Read requirement by requirement against the tree M10 closes on, the row's twenty requirements come
out **nine satisfied, three unmet, eight partial**, with the evidence per row in
`src/save/README.md`. A Complete cell costs **eleven pieces of work, not the two M9 named**:

- **Integrity and confidentiality** — integrity is complete; confidentiality is absent. It needs a
  vetted AEAD, `thirdparty-dependencies` names mbedTLS as this engine's cryptography library, and
  adopting a dependency *"SHALL go through the OpenSpec change flow recording the evaluation against
  these criteria"* — so it is a change of its own with a key-management story attached.
- **Save diagnostics and inspection** — there is no inspector, nothing answers "why is this field in
  the save", and there is no semantic diff.
- **Forbidden save patterns** — ten of them, *"each SHALL be checkable"*, and none is checked by any
  tool, criterion or grep.
- Among the partials: the **large-world save benchmark the requirement says the engine "SHALL
  maintain"** does not exist (`benchmarks/` has no save entry at all);
  `LoadFailure::UnresolvableReference` is declared and produced by nothing; the ordered load pipeline
  is assembled inside `samples/06-open-world/` rather than in the engine; and **no engine module
  above `src/save/` links `cy::save`** — the only translation between `world::PersistenceOverlay` and
  `save::Overlay` in the tree is about ninety lines in that sample.

**This proposal does not decide what to do about it.** It records it where the next scoping pass
cannot miss it, which is what §4 asked for.

## What Changes

**Nothing is scoped yet, and that is deliberate.** This change is *opened* by M10's closing gate so
the ladder continues as a deliberate act — `m10:m11-open` is the criterion that requires it, the same
shape as `m8c:m9-open` and `m9:m10-open` — and the scoping decisions below have to be taken before a
task list means anything.

### The decision this milestone cannot avoid: is M11 one milestone or two?

[The capability matrix](../../../docs/roadmap/capability-matrix.md#milestone-load) has said since M6
that M11 *"is the one milestone that could reasonably be split, and the roadmap will split it through
a change if the work turns out to be separable along a real seam rather than an arbitrary one"*.
**The load has grown twice since that sentence was written, both times at M10's gate:**

- M10's audit of four earlier closed milestones' columns moved **thirteen Complete cells** here
  (`m9:record-matches-plan-history`, M10 task 6.5): 48 → 61.
- M10's own closing gate moved **four more** — `save-and-persistence`, `navigation`,
  `rendering-global-illumination` and `world-partition-and-streaming`: 61 → **65**.

So M11 now completes 65 of the 76 capabilities in the set, of which **three are still at Seed**
(`editor-architecture`, `live-editing`, `ml-inference`) and one, `xr-support`, is deferred by
decision. **Seventeen of the sixty-five arrived here at M10's gate alone**, because a gate refused a claim
rather than because anyone planned the work here; earlier gates moved more.

**The seam, if there is one, is visible in what those seventeen have in common.** Roughly: an
*editor and authoring* group (`editor-architecture`, `live-editing`, `project-and-plugins`,
`editor-viewport-and-gizmos`, `material-compiler`, `shader-system`, `asset-import-pipeline`), a
*platform and reach* group (the two backends, the porting surface, mobile, packaging, patching,
`build-system-and-platforms`, `core-platform-abstraction`) and a *finishing* group (everything that
has been Working for several milestones and needs its last requirements). The platform group is the
one `delivery-roadmap`'s M11 row is actually about; the other two are accumulated debt. **Splitting
along that line is a real seam and splitting by count is not**, and this change should decide it
before it decides anything else.

### The scope M11 was written against, unchanged until that decision is taken

| Capability | → | Scope |
|---|:---:|---|
| `rhi-and-render-graph` | C | **Metal** (native, not a translation layer) and **D3D12** to parity with Vulkan |
| `build-system-and-platforms` | C | Cross-compilation, the **porting surface**, mobile targets, distribution artefacts, the full CI matrix |
| `core-platform-abstraction` | C | A **native** `Platform` and `DisplayServer` backend for one desktop platform, replacing SDL3 there and proving the abstraction carries no SDL assumption |
| `build-and-packaging` | C | Content audit, provenance and symbols, downloadable content, distributed execution |
| `rendering-forward-clustered` | C | Mobile pipeline differences, MSAA, multi-view |
| `xr-support` | — | Prerequisites verified and held open; XR itself remains deferred |
| `testing-and-quality` | C | The full gate set, the documentation gate |
| Everything else | C | Every remaining requirement, or an explicitly recorded deferral |

## Capabilities

Sixty-five capabilities to **Complete**; `xr-support` deferred with its prerequisites held open. The
full column is [the capability matrix](../../../docs/roadmap/capability-matrix.md); the M11 row of
[`docs/ROADMAP.md`](../../../docs/ROADMAP.md#m11--reach--the-10-gate) is the scope statement this
change implements.

## Impact

- **New code**: two graphics backends, one native platform backend, the mobile targets, and the
  packaging and patching surface — plus whatever each of the sixty-five rows is short of.
- **Existing code**: every module with a requirement still unmet. The three declared gaps M10 hands
  forward all point at the same absent thing — `cy/field.slang` and a shader-side field sampler —
  which is one piece of work that closes `fields-sampled-on-a-device` and is the only credible route
  to `world-frame-budget`.
- **Closing artefact**: `samples/11-ship` — one project built, cooked, packaged and launched on every
  supported target from a single recipe.
- **Risk**: the two backends are the named risk of the milestone as planned. The **unnamed** risk is
  the load: sixty-five Complete cells in one milestone is the estimate `delivery-roadmap` itself
  calls *"an unanalysed estimate"* when a capability jumps to Complete in one milestone, and
  seventeen of the sixty-five landed here at M10's gate alone, because a gate refused a claim rather
  than because anyone planned the work here.
