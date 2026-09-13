# M11 — Reach: the same project, everywhere, from one command

> **SUPERSEDED BY FIVE RUNGS, AND KEPT AS THE RECORD OF WHY.** Task 0.1 below — *is M11 one
> milestone or two?* — is **answered**: M11 is **five**. The work is scoped in
> [`implement-m11a-foundations`](../implement-m11a-foundations/proposal.md),
> [`implement-m11b-authoring`](../implement-m11b-authoring/proposal.md),
> [`implement-m11c-image`](../implement-m11c-image/proposal.md),
> [`implement-m11d-desktop`](../implement-m11d-desktop/proposal.md) and
> [`implement-m11e-ship`](../implement-m11e-ship/proposal.md), each with its own ledger under
> `tools/roadmap/milestones/`, its own gate in `tools/roadmap/gates.toml` and its own floor in
> `selftest.MINIMUM_CRITERIA`, as M8's three rungs have.
>
> **This change is not re-scoped into one of them and is not deleted.** It is the record of what M11
> inherited, what was found about it, and what the split was decided against — evidence the five
> rungs are built on and which none of them restates in full. The scope table below is the plan M11
> was written against; **the rung that owns each row is named beside it**, and where the two differ
> the rung wins.

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
| `m9:record-matches-plan-history` | M9, **re-declared at M10's gate** | four closed milestones' columns claim Working for rows the status record holds at Seed — M3 `testing-and-quality`, M4 `build-system-and-platforms`, M6 `developer-workflow-and-just`, M8.b `thirdparty-dependencies`. M10 task 6.5 closed this gap by RECORDING all four, and M10's closing gate put the record back: **no criterion in any of the fifteen ledgers evaluates one of these rows.** The only `expect_tiers` entry naming any of them is `m0:roadmap-tiers` expecting `seed`, and an exit tier is a FLOOR, so nothing could ever contradict a Working claim. Closing it is one of two deliberate acts — write criteria that *evaluate* the four rows and then record them, or move the four cells the way the other fifteen moved |

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

**This proposal did not decide what to do about it; the split does.** The row is **M11.a's**, and
M11.a's own proposal names it as the row that rung predicts it will demote — re-scoped through a
change against its own specification *before* it is recorded either way, which is what
`delivery-roadmap` requires of a capability demoted twice. A third demotion would not mean it is late
either.

## What Changes

**This change was opened unscoped, on purpose**, by M10's closing gate — `m10:m11-open` is the
criterion that required it, the same shape as `m8c:m9-open` and `m9:m10-open` — because the scoping
decisions below had to be taken before a task list meant anything. **They have now been taken**, and
what this section records is the question, the evidence, and the answer, in that order. The work
itself lives in the five rung changes.

### The decision this milestone could not avoid: is M11 one milestone or two?

[The capability matrix](../../../docs/roadmap/capability-matrix.md#milestone-load) has said since M6
that M11 *"is the one milestone that could reasonably be split, and the roadmap will split it through
a change if the work turns out to be separable along a real seam rather than an arbitrary one"*.
**The load has grown twice since that sentence was written, both times at M10's gate:**

- M10's audit of four earlier closed milestones' columns moved **thirteen Complete cells** here
  (`m9:record-matches-plan-history`, M10 task 6.5): 48 → 61.
- M10's own closing gate moved **four more** — `save-and-persistence`, `navigation`,
  `rendering-global-illumination` and `world-partition-and-streaming`: 61 → **65**.

So M11 now completes 65 of the 76 capabilities in the set, of which **seven are still at Seed**
(`build-system-and-platforms`, `developer-workflow-and-just`, `editor-architecture`, `live-editing`,
`ml-inference`, `testing-and-quality`, `thirdparty-dependencies`) and one, `xr-support`, is deferred
by decision. **Four of those seven are the four rows of `m9:record-matches-plan-history` above** —
their columns claim Working at a closed milestone and the record holds Seed — so four of the Complete
cells M11 owes sit on rows whose *Working* tier is itself unchecked, and three of those four
(`build-system-and-platforms`, `developer-workflow-and-just`, `thirdparty-dependencies`) are on the
platform seam this milestone is actually about.

**Seventeen of the sixty-five arrived here at M10's gate alone**, because a gate refused a claim
rather than because anyone planned the work here; earlier gates moved more.

**The seam, if there is one, is visible in what those seventeen have in common.** Roughly: an
*editor and authoring* group (`editor-architecture`, `live-editing`, `project-and-plugins`,
`editor-viewport-and-gizmos`, `material-compiler`, `shader-system`, `asset-import-pipeline`), a
*platform and reach* group (the two backends, the porting surface, mobile, packaging, patching,
`build-system-and-platforms`, `core-platform-abstraction`) and a *finishing* group (everything that
has been Working for several milestones and needs its last requirements). The platform group is the
one `delivery-roadmap`'s M11 row is actually about; the other two are accumulated debt. **Splitting
along that line is a real seam and splitting by count is not**, and this change decided it before it
decided anything else.

**The three groups above were the first guess and the answer is close to them but not the same.**
Grouping by *what the rows are about* put `material-compiler` and `shader-system` with the editor,
and left "finishing" as a bucket rather than a claim. Grouping by *what an artefact can refute*
moved them to the image rung — because image quality is expressed through them — and dissolved the
finishing group entirely: every row in it belongs to whichever artefact would show it was not
finished. The five rungs below are that grouping, and the difference between the two is the whole
content of the rule this change adds.

### The decision, taken: five rungs, and the seam each one is judged on

The seam is **the artefact**, not the count. `split-m8-authorable-and-systems` added the rule that a
milestone whose closing artefact cannot be reached without its own risk spike succeeding contains
two; that rule is about **risk**, and M11 is not blocked by one spike. The rule this split adds is
the sibling: **a milestone whose scope cannot be judged by one closing artefact is several
milestones sharing a number, because a gate that cannot name what it is looking at is not a gate.**
Splitting by count is refused by the same rule — a rung has to be a claim an artefact can refute.

| Rung | Rows | Reqs | For | Closing artefact |
|---|---:|---:|---|---|
| **M11.a** · Foundations | 12 | 233 | the seven inherited gaps, the frame budget, the cross-leg digest job, `save-and-persistence` re-scoped | the world demo inside 16.7 ms on a device, streaming |
| **M11.b** · Authoring | 24 | 420 | `editor-architecture` and `live-editing` off Seed, the editor finished, the gameplay rows | **a real sample game**, made through the editor |
| **M11.c** · Image | 15 | 232 | `material-compiler` and `shader-system` first, then the eight rows the picture is made of | **an art-directed beauty shot**, authored through that editor |
| **M11.d** · Desktop | 10 | 129 | Metal native, D3D12, a native `Platform` and `DisplayServer`, the gates | `samples/11-ship` on desktop |
| **M11.e** · Ship | 4 | 55 | mobile, the full matrix, distribution, the sweep, the 1.0 record | `samples/11-ship` everywhere, and the 1.0 record |

**And the ordering is the argument, not a convenience.** The editor is finished before the picture is
art-directed: a beauty shot assembled by hand in C++ proves the renderer and nothing else, while one
authored *through* the editor proves both, and is the honest demonstration of a usable engine. Mobile
is last because it is the only scope on the ladder this project cannot evaluate on a machine it owns.

### The scope M11 was written against, with the rung that now owns each row

| Capability | → | Owned by | Scope |
|---|:---:|:---:|---|
| `rhi-and-render-graph` | C | **M11.d** | **Metal** (native, not a translation layer) and **D3D12** to parity with Vulkan |
| `build-system-and-platforms` | C | **M11.e** | Cross-compilation, the **porting surface**, mobile targets, distribution artefacts, the full CI matrix |
| `core-platform-abstraction` | C | **M11.d** | A **native** `Platform` and `DisplayServer` backend for one desktop platform, replacing SDL3 there and proving the abstraction carries no SDL assumption |
| `build-and-packaging` | C | **M11.d** | Content audit, provenance and symbols; downloadable content and distributed execution at **M11.e** |
| `rendering-forward-clustered` | C | **M11.e** | Mobile pipeline differences at M11.e; MSAA and multi-view at **M11.d**, which is why the **C** cell is M11.e's |
| `xr-support` | — | **M11.e** | Prerequisites verified and held open; XR itself remains deferred |
| `testing-and-quality` | C | **M11.d** | The full gate set, the documentation gate |
| Everything else | C | the four other rungs | Every remaining requirement, or an explicitly recorded deferral — **which only M11.e may record** |

## Capabilities

Sixty-five capabilities to **Complete**; `xr-support` deferred with its prerequisites held open.
**Spread across five rungs**, each row owned by exactly one of them — 12 at M11.a, 24 at M11.b, 15 at
M11.c, 10 at M11.d and 4 at M11.e, carrying 233, 420, 232, 129 and 55 requirements respectively, 65
rows and 1,069 requirements in total. The allocation is
[the capability matrix](../../../docs/roadmap/capability-matrix.md), which now has **five M11
columns** rather than one, and [`docs/ROADMAP.md`](../../../docs/ROADMAP.md) carries a section per
rung with its own work table, artefact, exit criteria and spike.

## Impact

- **New code**: two graphics backends, one native platform backend, the mobile targets, and the
  packaging and patching surface — plus whatever each of the sixty-five rows is short of.
- **Existing code**: every module with a requirement still unmet. The three declared gaps M10 hands
  forward all point at the same absent thing — `cy/field.slang` and a shader-side field sampler —
  which is one piece of work that closes `fields-sampled-on-a-device` and is the only credible route
  to `world-frame-budget`.
- **Closing artefact**: five of them, one per rung, ending in `samples/11-ship` — one project built,
  cooked, packaged and launched on every supported target from a single recipe — and the **1.0
  record**.
- **The machinery of the split**, which this change's own tasks 0.1 cover: `record.MILESTONES` loses
  `m11` and gains `m11a` … `m11e` **in position**; five ledgers under `tools/roadmap/milestones/`;
  five gates in `gates.toml` at `joins-on-close`; five floors in `selftest.MINIMUM_CRITERIA`; five
  columns in the capability matrix and five rows in its load table; five sections in
  `docs/ROADMAP.md`; the M11 subgraph of `docs/roadmap/dependencies.md`; and one register entry in
  `docs/roadmap/risks.md` carrying the five spikes.
- **The seven inherited gaps are re-pointed, not deleted** — six at `m11a` and
  `record-matches-plan-history` at `m11e`, because it cannot pass until the last of its four rows is
  evaluated and recorded and two of those four are M11.d's. `criteria._check_known_gap` refuses a
  `known_gap_closes` the ladder does not carry, so the re-point and the `MILESTONES` edit are one
  commit or neither.
- **Risk**: the two backends are the named risk of the milestone as planned. The **unnamed** risk was
  the load — sixty-five Complete cells in one milestone is the estimate `delivery-roadmap` itself
  calls *"an unanalysed estimate"*, and seventeen of them landed here at M10's gate because a gate
  refused a claim rather than because anyone planned the work here. **The split does not reduce that
  load; it makes it judgeable**, which is the same thing M8's split did for M8's risk and no more
  than that. The risk the split ADDS is its own: if the rungs are not separable along their
  artefacts, the ladder has gained four gates that cannot close independently. `docs/roadmap/risks.md`
  entry 12 records it.
