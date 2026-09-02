# Design: CyberTexture and CyberShadow

## 1. Shared policy, separate storage

Four subsystems now maintain page caches: virtual geometry, virtual textures, virtual shadows, and
the illumination caches. Each independently invented the same vocabulary — request, priority,
residency, budget, eviction, prefetch — which is a strong signal that the policy is one thing.

It is equally a trap. Their storage genuinely differs: a geometry page is immutable and
content-addressed; a texture page may be produced at runtime; a shadow page is *rendered*, is
invalidated by unrelated objects moving, and is worth keeping precisely because regenerating it is
expensive. Forcing one physical cache on all of them would produce a generic structure that serves
none well.

So `residency` owns **policy and observability** — importance, scoring, budgets, eviction rules,
hysteresis, deadline propagation, pressure response, and the diagnostics — and each subsystem owns
its **storage and its update mechanism**. The rule is stated as a requirement rather than left as a
convention, because the natural drift is for a shared policy layer to grow a shared implementation.

## 2. One notion of what matters

The engine currently has at least five: virtual geometry's per-object importance, illumination's
importance, animation LOD's, audio's importance tiers, and foliage's gameplay-relevant species.
Each is defensible; together they mean a hero unit can be important to three systems and ordinary
to two.

**Render importance** becomes one value, published per instance in the GPU scene and consumed by
geometry detail, texture priority, shadow page resolution and freshness, animation rate, and
illumination quality. A hero unit is important once.

This is the difference between a collection of independent quality knobs and a coordinated quality
system, and it is cheap to specify now and impossible to retrofit later.

## 3. Residency models are a property, not a type

A texture is `Resident`, `StreamedMip`, `VirtualStreamed`, or `VirtualRuntime`, and the public
handle is the same in every case. Gameplay loads a texture; the material compiler knows how it is
sampled.

Two of those models matter for reasons worth stating. `StreamedMip` is retained deliberately —
conventional mip streaming is simpler and cheaper for ordinary assets, and virtualising everything
would be a regression for the majority of textures in most projects. `VirtualRuntime` is
first-class rather than a terrain feature: terrain composition, decal accumulation, world state, and
procedural surfaces are the same mechanism, and building it as "terrain's texture system" would mean
building it twice.

## 4. A resident tail, again

Virtual geometry guarantees an always-resident root so an object is never absent, only coarse.
Virtual texturing guarantees an always-resident **mip tail**, so a surface is never a black
checkerboard, only blurry. Virtual shadows guarantee a fallback chain ending in *unshadowed* rather
than a stall.

The same decision three times, and it is the decision that makes a virtualised system shippable: the
system degrades along a defined axis and never fails to produce a frame.

## 5. Prediction handles latency; feedback handles accuracy

GPU feedback is exact — it reports the pages a shader actually sampled — and it is inherently late,
because the request happens after the frame that needed it. Prediction is early and imprecise.

Neither alone is sufficient, and the division of labour is stated so that neither is asked to do the
other's job: **prediction covers latency, feedback establishes accuracy.** A region approaching from
the world's streaming prediction gets coarse pages prefetched; feedback then refines to the exact
pages the frame samples.

Feedback is compacted and deduplicated **on the GPU** before the CPU sees it. A per-pixel request
stream is not a design, and feedback density is itself a quality lever that can be reduced under
pressure.

## 6. Derivatives are a real problem under a visibility buffer

Virtual texture sampling needs texture-space derivatives to choose a mip and an anisotropy. In a
forward or conventional deferred pipeline the rasteriser supplies them. In the visibility buffer
pipeline, shading happens in a later pass from an instance and primitive identifier, and the
hardware derivatives available there are the derivatives of screen-space quads, not of the surface's
texture coordinates.

Reconstructed incorrectly, this produces either shimmering (mip too high) or blur (mip too low)
across the whole frame, and it is the kind of defect that gets attributed to the texture system for
months.

So analytic derivative reconstruction from the identified primitive is specified as a **renderer
requirement** of the visibility buffer path, not as something the texture system hopes for.

## 7. Shadow pages exist because a pixel needs them

The conventional model allocates shadow resolution per light and renders whatever casts into it.
The virtual model inverts this: visible pixels are projected into light space on the GPU, the pages
they touch are marked, and only those pages are allocated and rendered.

That inversion is the entire efficiency argument. A light illuminating a large volume of which two
square metres are visible renders two square metres of shadow.

It also makes caching worthwhile, because a page that was correct last frame is still correct unless
something in it moved — which turns the shadow problem from "render shadow maps" into "keep a cache
valid", a much better problem.

## 8. Caching is only as good as the invalidation, and the invalidation is only as good as the snapping

Two things make the cache work, and both are easy to get wrong:

**Precise invalidation.** The GPU scene already stores current and previous transforms and bounds,
so a moved caster's old and new bounds project into light space and dirty exactly the pages they
touch. Invalidating a whole light because one object moved would make the cache pointless.

**Clipmap snapping.** A directional light's clipmap must be quantised to page boundaries in world
space. Without it, moving the camera two centimetres shifts every clip level and dirties the entire
cache every frame — a system that looks like it caches and never does.

The harder invalidation case is deformation that does not move a transform: wind, vertex animation,
skinning. That is handled by a declared **deformation mode** on the geometry, with bounded
deformation expanding the bounds rather than dirtying every frame, and always-dirty reserved for
content that genuinely cannot be cached.

## 9. Shadow geometry does not need camera geometry

A shadow page's texels are much larger than a screen pixel, so the geometric error acceptable when
rasterising into it is much larger. Virtual geometry already selects detail from a continuous error
metric, so shadows simply ask for a different target: primary visibility at sub-pixel error, hero
shadows at about one shadow texel, background shadows at several.

This is one of the larger performance opportunities in the whole renderer, and it exists only
because geometry detail was specified as a continuous function rather than as discrete levels.

## 10. The cycle between shadows and virtual textures, broken deliberately

This is the failure the two systems create together, and it is not obvious until both exist:

- Rendering a shadow page for masked geometry needs the opacity mask.
- The opacity mask may live in a virtual texture.
- Virtual texture residency is driven by feedback from visible shading.
- Visible shading needs shadows.

Left alone this either deadlocks or produces shadows that flicker as opacity pages arrive.

It is broken in three places at once, because any single fix is fragile:

1. A material's **shadow program** is a separate compiled program carrying only what shadow
   rasterisation needs — opacity and deformation — not the primary program's full input set.
2. The textures that program samples are **shadow-critical**, and a coarse representation of them is
   guaranteed resident, the same guarantee as the mip tail but declared rather than incidental.
3. Shadow rendering **never waits** for a texture page. A missing fine page uses the resident coarse
   one, and the page is marked for refresh.

The general rule this expresses: no residency system may block on another residency system within a
frame.

## 11. Staleness is a budget lever, not a defect

Not every dirty page must be re-rendered this frame. A slowly moving distant tree's shadow tolerates
being several frames old; a character's contact shadow does not.

Pages therefore carry an **update class** and an age, and the budget controller spends its allocation
on the pages where staleness would be visible. This is the same shape as the illumination system's
confidence: a value that says how much to trust what is cached, driving where effort goes.

Doing this *instead of* a hard cap on pages per frame is what avoids the alternative failure, where
a camera cut produces a twelve-millisecond spike because every page became dirty at once.

## 12. Static and dynamic page separation is a benchmark, not a mandate

Separating a page's static content from its dynamic content — so a character walking past a city
does not re-render the city's shadow — is attractive and costs memory and bandwidth for two
representations.

It is specified as an **optional strategy with a declared trade-off**, to be adopted on evidence,
rather than as a requirement. Specifying an optimisation whose benefit depends on content as though
it were architecture is how engines acquire complexity nobody can remove.

## 13. Conventional shadows are not deleted

Mobile and low-end profiles need conventional cascades and atlases, baked shadows remain the right
answer for static content on constrained hardware, and the conventional path is the fallback when
capabilities are missing.

So shadow mode is per light and per profile — none, baked, conventional, virtual, ray traced,
hybrid — and the existing atlas and cascade requirements remain as the conventional path rather than
being replaced.

## 14. Deadlines propagate from one prediction

When the world predicts the camera reaching a region in some hundreds of milliseconds, that single
prediction should become a geometry page deadline, a texture page deadline, a shadow warm-up hint,
an audio preload, and an illumination prefetch.

Each subsystem discovering the same future independently is both wasteful and inconsistent — they
would disagree about how soon. One prediction, propagated as deadlines through the residency layer,
is what makes a large world arrive smoothly rather than in pieces.

## 15. Build order

| Phase | Texture | Shadow |
|---|---|---|
| 1 | Conventional textures: cook, mips, compression, bindless, mip streaming | Conventional cascades, spot and point maps, filtering |
| 2 | Virtual address spaces, page tables, physical cache, mip tail, offline tiles | Local light virtual pages, receiver marking, dirty-page rendering |
| 3 | GPU feedback, compaction, priority | Directional clipmaps with snapping |
| 4 | Asset and I/O integration, content-addressed tiles, budgets | GPU-driven caster culling through virtual geometry |
| 5 | Runtime producers: terrain, decals, world state | Persistent caching and precise invalidation |
| 6 | Predictive streaming, shared residency, adaptive feedback | Budgets, update classes, adaptive geometry error |
| 7 | — | Foliage and material specialisation, shadow programs |
| 8 | — | Hybrid ray-traced refinement |

**Phase 1 is deliberately conventional in both columns.** Virtualising before ordinary textures and
ordinary shadows work well produces a system whose bugs cannot be isolated, because there is no
correct reference to compare against.

## 16. Gaps

With these two specified, the gaps that remain are ones no capability is currently working around:
**weather**, **hydrology and erosion**, and **procedural generation** beyond foliage rules. Each is
recorded where it is relevant, and each has its inputs and outputs already expressed as environment
fields.

Hardware sparse-resource residency differs meaningfully between Vulkan, Metal, and D3D12; the
capability model already requires querying rather than assuming, and virtual texturing is specified
so that a software page table works where hardware sparse residency does not.
