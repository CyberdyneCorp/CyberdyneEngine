# `src/rendering/denoise/` — layer 4

CyberDenoiser: the **one** accumulation and edge-aware filter every stochastic signal goes through —
indirect diffuse, indirect specular, ray-traced shadows, ambient occlusion and stochastic direct
lighting.

**Governed by**: `denoising`, at **Working** for M7. Task 9.3.

## The files

| File | What it holds |
|---|---|
| `denoiser.h` | `SignalKind`, `SignalConfig`, `GuidanceBuffers`, `HistoryGuidance`, the priced quality ladder, the per-signal `Diagnostics`, and `Denoiser` |

## The rule this module exists to keep, and how it is kept

**`SignalKind` carries no behaviour.** It selects a `SignalConfig` and a history slot, and nothing
else in `denoiser.cpp` reads it. Everything that differs between the five signals is a field of that
config: the domain (radiance or visibility), the lobe shape, the history length, the cascade length,
the four edge-stopping tolerances, and how much roughness widens the filter.

`tests/test_one_filter.cpp` is what makes that structural rather than aspirational. It denoises the
same buffer under two different signal kinds carrying the same configuration and asserts the two
images are identical **bit for bit** — not "within tolerance", because a tolerance is where a second
filter hides. Any per-signal special case anywhere in the implementation fails that case on the day
it is written. The file's second case is the control: two kinds with *different* configurations must
produce different images, or the first would pass on a filter that ignored its configuration too.

## Four things worth knowing before changing anything here

**This module computes no history.** `denoising` requires that temporal accumulation use the
framework in `temporal-rendering` and that denoising "SHALL NOT implement its own history handling".
That is expressed as an input: `HistoryGuidance` is the reprojection's answer — for each pixel,
which pixel of the previous frame it came from and how much of that history survived validation.
Nothing here computes a motion vector, reprojects, or decides what a disocclusion is. The seam is a
struct of spans rather than a link dependency because the temporal framework is written in the same
milestone by another hand, and a struct of spans is the interface either can fill.

**Visibility terms are not colours.** A shadow or an ambient occlusion term is reconstructed as
occlusion: the edge-stopping term compares the occlusion values directly and at a much tighter
tolerance, so a penumbra edge stops the filter the way a geometric edge does. Blurring it as a
colour is what loses contact hardening, and `SignalDomain` is the one field that prevents it.

**The history fed back is the accumulated signal, not the filtered one.** Both are defensible and
this one is honest: feeding the spatial filter's output back would make the accumulated variance an
estimate of the filter's residual rather than of the signal's noise — and the variance is what
drives the filter, so a converged region would report the variance it has *after* filtering and
widen no filter it needed.

**The quality ladder is priced.** `quality_ladder()` gives four positions with a `relative_cost`
against position 0, which is the field `design.md` §2.10 found the renderer budget arbiter needs in
order to price a step. Position 3 still denoises: a ladder whose last rung is "off" would make the
budget's last step a visual discontinuity rather than a coarser frame.

## What it does not depend on

No device, no render graph, no shader, and no temporal module. `cy::core-jobs` is linked for one
function, the monotonic clock that stamps the per-signal cost the diagnostics requirement asks for.
Every case in `tests/` runs headless.
