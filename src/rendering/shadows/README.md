# `src/rendering/shadows/` — layer 4

CyberShadow: shadow maps as sparse virtual address spaces whose pages exist because a visible pixel
needs them.

**Governed by**: `virtual-shadows`, at **Working** for M7. Tasks 8.1 and 8.2.

## The files

| File | What it holds |
|---|---|
| `address_space.h` | shadow modes, the three projections, the packed `VirtualPage`, `address_of()`, the texel footprint, level selection, and the box-to-pages projection everything else is built from |
| `clipmap.h` | directional clipmap levels, **snapped to page boundaries in world space**, and `clipmap_pages_entering()` — the band a boundary crossing exposes |
| `pages.h` | receiver-driven marking and compaction, the four update classes, staleness priority, and the selection that spends an allocation instead of capping pages per frame |
| `cache.h` | the physical page cache: residency, dirty state with its cause, age, pinning, cost-weighted eviction, and the statistics the profiler reads |
| `invalidation.h` | precise invalidation from previous and current bounds, the four shadow deformation modes, and the light and streaming paths |
| `budget.h` | the five declared levers in their reduction order, each with a **relative cost per position**, and the controller that tightens locally and relaxes only on the arbiter's grant |
| `bias.h` | bias derived from texel footprint, slope and geometric error, and the ledger that reports overrides |
| `fallback.h` | the chain — requested, coarser, stale, approximation, unshadowed — and the record of which rung was taken |

## Six things worth knowing before changing anything here

**Snapping is a requirement, not an optimisation, and it is to a PAGE.** The specification says so in
as many words: without it, small camera movement invalidates the entire cache every frame, producing
a system that appears to cache and never does. Snapping to a texel would be the more obvious choice
and is wrong — the cache's unit is a page, so a texel-snapped origin shifts every page's world
footprint while looking, in a debug view, as though it had barely moved.

**A page id is the identity, and the sort key is that id.** Two marks of one page must collide or
compaction has nothing to merge; two runs over one frame's marks must produce the same list in the
same order, because the render order of dirty pages decides which of them fit inside a budget.

**There is no pages-per-frame cap.** A hard cap turns a camera cut into a stall. The allocation is
spent by `staleness_priority()`, `Critical` pages are never left behind, and everything else waits
its turn with its stale contents still on screen.

**Invalidation dirties the previous bounds as well as the current ones.** Dirtying only where the
caster now is leaves its old shadow painted on the world. `invalidate_caster_motion()` is two
projections and nothing else, which is what keeps "a character walks across a city" costing a
handful of pages.

**The budget controller cannot see the frame, and cannot relax on its own.** `report_measured_ms()`
takes the subsystem's own cost and there is no second setter. `design.md` §2.6 found the unwritten
half of the rule: a controller that steps back up is spending time that comes out of the frame,
which is the quantity it is forbidden to see, so `grant_relax_step()` comes from the arbiter.

**The ladder declares RATIOS; the controller measures the scale.** `ShadowLeverLadder::relative_cost`
is `design.md` §2.10's "exactly one thing `residency` needs added" — what each ladder position costs
relative to position 0. `base_cost_ms` is only the estimate before the first measurement arrives; a
declared absolute cost would be wrong the moment the scene changed.

`residency::LeverSchedule` now carries a `relative_cost` of its own, added by this milestone's
arbiter work. They are the same idea at two granularities, not two mechanisms: `residency`'s is
indexed by pressure level (three positions, six named levers) and this one by ladder position (up to
four, five levers including geometry error and contact refinement, which are not expressible as
memory pressure). The shadow budget is arbitrated in **milliseconds** by the renderer's budget
arbiter rather than in bytes by the pressure monitor, which is why the finer ladder lives here.
Folding the two into one declaration is worth doing and is a change to `residency`'s enumeration
rather than to this module — so `budget.h` names it rather than doing it.

## What is here and what is not

No device, no render graph, no shader — the same division `src/rendering/culling/` and
`src/rendering/lighting/` draw. What is here is every decision about **which** pages exist, **when**
they are re-rendered and **what** they cost, and each of those is arithmetic over positions, bounds
and frame counts. A shadow that swims, a cache that never hits, a controller that oscillates and a
fallback that stalls are all findable without a GPU, and `tests/` finds them headless.

What is **not** here, and is the device half of `virtual-shadows`: the compute dispatch that marks
pages from the depth buffer, GPU-driven caster selection against page frusta, page-batched indirect
rasterisation, and the filtering kernels. The requirement that those be GPU-driven is a statement
about where a loop runs, and the interfaces here are shaped so that the loop is a dispatch rather
than a scene walk — **nothing in this module takes a scene, a light list or an object.**
`PageRequestSet::mark()` takes a receiver sample, which is what a shader has after reading one pixel
of the depth buffer.

`ShadowMode::RayTraced` and `Hybrid` are named by `address_space.h` and are not implemented here;
they are `ray-tracing-infrastructure`'s to supply and section 9's to land. The fallback chain's
`Approximation` rung is the seam: it is a flag a caller sets, and the caller is the pass that knows
whether a trace is available this frame.

## The test suites

`render_shadows` is the unit suite: the address space, the clipmap snapping, the page cache,
invalidation, the fallback chain and the bias derivation, all in microseconds.

`render_shadow_control` is the integration suite, and three things live there because shrinking them
to fit a one-millisecond budget would certify a different scenario from the one the requirement is
about:

* the **control-law sweep** below;
* the **camera cut** — two thousand dirty pages against an allocation that affords about a hundred,
  which is the case `virtual-shadows` forbids a fixed page cap for ("a hard cap turns a camera cut
  into a stall");
* **teardown while full** — a cache destroyed with every slot live, pinned and dirty; a request set
  destroyed after its overflow path was taken; and an invalidation whose scratch is deliberately too
  small. Nothing in this module is threaded, so the load that can go wrong at teardown is occupancy
  rather than concurrency, and the assertion is the sanitizers: the suite is run under ASan with
  leak detection, under UBSan, and under TSan.

## The control law, and how it was certified

`tests/test_budget_sweep.cpp` runs **41 load magnitudes** from ×1.00 to ×2.40, holds each for 800
frames and counts every lever change in the last 450. That is `design.md` §2.1's methodological
finding applied one level down: the levers are a discrete ladder, so where the equilibrium lands
relative to a ladder boundary decides whether the loop pulses, and one step magnitude can make any
law look stable. The arbiter spike's own first draft passed a single load and oscillated on 27 of 71.

The margins are the spike's: tighten above `allocation × 1.06`, relax only when the prediction is at
or below `allocation × 0.88`, EMA α 0.25, relax dwell 6 frames. The sweep's companion case removes
the tighten margin and measures the ratchet it produces at nominal load, so the number stays a
measurement rather than a tradition.

**What the sweep certifies and what it does not.** The cost model is the declared ladder's own
arithmetic plus ±3 % measurement noise. So it certifies the **control law** — that the loop settles
on every load, never settles over budget, reaches its minimum without disappearing, and returns
authored quality when the load goes away. It does **not** certify that the declared relative costs
describe a real shadow pass; that needs a device and a measured pass, and it is stated here as a
limit rather than implied away.
