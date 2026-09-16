# `src/rendering/sky_illumination/` — layer 4

The composition point where the physical atmosphere becomes illumination: the `gi::SkyTerm` fitted
from `sky::Atmosphere`, the incremental refit a moving sun triggers, the invalidation that refit
files, the sun the atmosphere implies, and the cloud shadow field read into it.

**Governed by**: `rendering-global-illumination`, and by `atmosphere-sky-and-clouds`' requirement
that the coarse cloud shadow field be consumed by illumination. M11.c tasks 2.1–2.4.

## Why this module exists at all

[Dependency cycle 2](../../../docs/roadmap/dependencies.md) is entirely about this seam: an analytic
sky seeds at M7, *"the physical atmosphere, its precomputed tables and volumetric clouds land at M10,
and GI reaches Complete there"*. **The atmosphere landed at M10 and the seam was never joined.**

That was a checkable fact rather than an impression:

* `src/rendering/gi/` linked `cy::rendering-denoise` and `cy::rendering-raytracing` and not
  `cy::rendering-sky`.
* `sky_light.h` carried the adapter *in a comment* and said a composition point writes it.
* **Nothing in the tree constructed a `gi::SkyTerm` from an atmosphere**, so all thirteen scenarios
  under `rendering-global-illumination`'s "Sky and atmosphere" were satisfied by a two-colour
  gradient, with no check able to say so.

## Why it is a third module rather than an edge added to one of the two

Because both of the existing edges cost something specific and neither cost is worth paying.

| Direction | What it would cost |
|---|---|
| `cy::rendering-sky` → `cy::rendering-gi` | `src/rendering/sky/CMakeLists.txt` refuses it in capitals: *"a sky that depended on a global illumination system could not be tested without one"*. `fit_sky_gradient`'s 12.4 % agreement with the atmosphere's own irradiance integral is measured in that module's suite **with no GI present**, which is what makes "sufficient for GI's sky term" a measurement |
| `cy::rendering-gi` → `cy::rendering-sky` | `cy::rendering-gi` links two modules and every case in its suites runs headless. The sky drags `cy::environment`, `cy::world`, `cy::rendering-post`, `cy::rendering-temporal` and `cy::rendering-arbiter` behind it, into a module whose README ends with "What it does not depend on" |

So: a third module that names both and that neither names. **Exactly one library in this tree depends
on `cy::rendering-gi` and `cy::rendering-sky` at once and it is this one** — which is what makes the
seam a fact about the link graph rather than a paragraph, and what
`m11c:gi-sky-term-constructed` checks.

## It is not one adapter, and that is the measured answer to `dependencies.md`

`dependencies.md` calls joining this *"one adapter at one composition point"*. The adapter is
`SkyIllumination::fit()` and it is four lines. The requirement's second half is the rest of the file:

**Incremental where the sky changes continuously.** `IlluminationSystem::configure()` rebuilds the
clipmap, discards the probe cache and reconstructs the acceleration service — and until M11.c it was
the only way to change `IlluminationSettings::sky`. A sun that moved a quarter of a degree must not
cost that, so the term is refitted on a threshold and installed through the new `set_sky_term()`,
which writes the two places the sky is read from and nothing else.

**Invalidating only the illumination that depends on it.** A sun that rotated moved no geometry, so
the sparse distance field — a representation of where surfaces *are* — cannot have been invalidated
by it. `InvalidationCause::SkyChanged` is a cause of its own and
`IlluminationSystem::service_invalidations` skips the field for it and for nothing else.

**Under the illumination budget.** One fit per update, and the fit's direction count is a lever. The
cost of a day is therefore the number of threshold crossings rather than the number of frames.

Measured by `integration.rendering_gi_sky`, over 1400 frames of a sunrise on a twenty-minute day:

| | |
|---|---|
| gradient fits | **28** of 1400 frames |
| field bricks invalidated | **0** |
| worst irradiance step across a refit | **0.30 %** |
| irradiance moved across the take | **6.8 %**, more than twenty times the worst step |
| a full recomputation, measured beside it | **64 field bricks and 27 probes per frame** |

## The cloud shadow field, and the one function it is read through

`atmosphere-sky-and-clouds` requires the coarse field to be *"consumed by terrain, foliage, water,
and illumination"*. This module is the fourth consumer, and it reads through
`sky::CloudShadowField::sample()` — the static function the other three call — rather than through
`FieldStore::sample_at()` with a residency of its own. That is deliberate:
`m10:sky-field-round-trip` records a check that was moved **off** the defect it named by being
rewritten to read through the store directly, which left the function every consumer calls unjudged.
A consumer that picks its own sampling path is a consumer the field's own declared defect cannot
break.

**What the field attenuates is the sun, and not the sky term.** The field is the fraction of *direct*
sunlight reaching a position; cloud cover scatters the rest into the sky rather than deleting it, so
dimming the gradient by the same number would take that light out of the frame twice.

## The two things a published picture has to say, and where they come from

**Which sky lit it.** `SkyTermProvenance` is `AnalyticFallback` or `PhysicalAtmosphere`, and the
fallback is a first-class configuration rather than a failure — a project with a stylised sky is
entitled to one and entitled to have its captions say so. A gradient and a fitted atmosphere both
produce plausible pictures; this enumerator is the only thing that tells them apart afterwards.

**At what exposure.** `SkyIlluminationSettings::exposure` defaults to one, which is the physical
answer. Anything else means the picture's sky is the atmosphere's *colour* at the project's
*brightness* — a true sentence and a different one from "lit by a physical sky". The setting exists
because the atmosphere answers in lux and most authored content is not in that unit; the header
carries the measurement that made the case for it.

## What it does not depend on

No device, no render graph, no shader — as with both of the modules it composes. Every case in
`tests/` runs headless, which is what lets a seam this rung is judged on be judged on a machine with
no GPU.
