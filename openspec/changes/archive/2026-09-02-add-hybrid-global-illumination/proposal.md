# Hybrid global illumination

## Why

`rendering-global-illumination` currently specifies a layered but essentially **algorithmic**
answer: bake what is static, run a cascaded grid of runtime probes for what moves, and add
screen-space detail on top. Each layer is specified as an independent technique with its own
scene representation and its own cost.

That was a reasonable baseline. It is not what a modern illumination system looks like, and three
things about it will not survive contact with the rest of the engine:

**1. Each technique carries its own scene representation.** Dynamic GI traces "an acceleration
structure where available, or an SDF/voxel representation otherwise". Reflection probes capture
the scene themselves. Screen-space reflections march depth. Nothing states what the *world looks
like to an illumination query*, so every technique answers that question separately and they will
disagree — a reflection and a bounce from the same wall computed from two different
representations of that wall.

**2. A secondary ray that hits a surface has no cheap way to know its radiance.** With materials
now compiled from graphs that may sample eight textures and evaluate layered closures, evaluating
the full material at every secondary hit is not affordable. Without a cache, hybrid tracing is
theoretically nice and practically unusable.

**3. Diffuse GI and reflections are specified as unrelated systems.** They answer the same
question — what radiance arrives from this direction — with different ray distributions. Built
separately, they duplicate the tracing infrastructure, the caching, the denoising, and the budget,
and they will disagree at exactly the places a viewer notices.

The organising principle:

> **CyberGI is a fully dynamic, hybrid global illumination and reflection system that resolves
> indirect radiance using screen-space visibility, adaptive world-space radiance caching, software
> scene tracing, and hardware ray tracing, under one quality and GPU-time budget.**

The operative rule inside it: **use the cheapest source that can give a trustworthy answer**, where
"trustworthy" is a number the system computes, not a guess. Screen tracing answers most rays
almost free and cannot answer any ray that leaves the screen. World tracing answers the rest.
Confidence decides which is used, and drives whether spending more is worthwhile.

## What changes

**`rendering-global-illumination` is substantially reworked** into CyberGI: a **GI scene** that
defines what the world looks like to an illumination query, a **surface cache** holding shaded
radiance so secondary hits are a lookup rather than a material evaluation, a **radiance cache** of
adaptively placed probes with a priority-scheduled update budget, three **tracing tiers** (screen,
software, hardware) selected by confidence, a shared **resolve** producing both diffuse GI and
reflections, incremental invalidation, GI importance and volumes, far-field representation, and
diagnostics able to answer *why is this area dark*.

The existing baked path is not removed. It becomes a **GI mode** alongside the dynamic one, and
gains a second job: seeding the dynamic caches so a level looks right immediately rather than
converging from black.

**New capability `denoising`** — one framework for every noisy signal (GI, reflections, ray-traced
shadows, ambient occlusion), material- and geometry-aware through the visibility buffer's
identifiers, rather than each effect growing its own blur.

**`rendering-lighting-and-shadows`** gains light channels, and the **stochastic many-light path**
specified as an architected growth path with clustered lighting as the shipping default — because
thousands of shadowed area lights is a direct-lighting problem, not a GI one, and mixing them up
is how systems end up with two lighting models.

**`material-compiler`** gains **secondary material programs**: a material compiles to a primary
program, a cheaper secondary program for illumination queries, and a far-field constant
approximation. This is what makes the surface cache affordable.

## Impact

- **New**: `denoising`
- **Modified**: `rendering-global-illumination` (substantially), `rendering-lighting-and-shadows`,
  `material-compiler`, `rendering-post-processing`, `virtual-geometry`, `testing-and-quality`,
  `thirdparty-dependencies`
- **Not in scope**: **virtual shadow maps**. The brief mentions them as the sun's direct-shadow
  path; they are a shadow system, not an illumination one, and specifying them here would repeat
  the mistake this change corrects. They remain the recommended companion change.
- **World partition** is now required by a fourth subsystem. The GI scene is explicitly specified
  as cell-scoped and evictable, against a capability that does not yet exist.
