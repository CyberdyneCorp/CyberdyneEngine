# Design

## Context

| piece | where | state before this change |
|---|---|---|
| column optics | `water::water_transmittance`, `water_in_scatter`, `water_column_colour` (`src/water/shading.h`) | Beer-Lambert over absorption plus scattering and a single-scattering in-scatter, per channel; tested by `unit.water`; called once per frame by `samples/10-world` for a nominal 14 m column |
| surface closure | `water::build_closure` | Fresnel F0 from the index, foam roughness; called by nothing that draws |
| caustic tiers | `water::select_caustic_tier` | the surface-derived tier is the default; no tier is drawn anywhere |
| the sea in the frame | `samples/10-world` | one run of world.slang's Lambert path: a per-vertex colour (deep blue plus a noise "foam"), the glitter lobe, cloud shadows. No reflection, no refraction, one colour from the beach to the channel |

## Goals / Non-Goals

**Goals:**

- The sea reflects the sky and the land as they are drawn, weighted by Fresnel.
- Under the surface the bed shows through, attenuated by Beer-Lambert over the path from the surface
  to the bed and filled by the column's in-scatter, with the body's own coefficients: the fjord
  shades from green-gold over the shallows to the deep blue of the channel with no gradient
  authored.
- Foam along the shore, from the depth of the column.
- Caustics on the shallow bed, moving with the water, derived from the surface.
- With water shading off, the frame is the one before, byte for byte.
- Every one of those is a test that has been seen failing.

**Non-Goals:**

- The material system's water closure. `water` asks for water to be shaded "through a water surface
  closure in the material system"; `material-compiler` has no water node and this change does not
  add one. The shader is the sample's, as world.slang is, and it consumes src/water/'s numbers
  through `build_water_params()` rather than restating them.
- The reflection hierarchy (screen trace, world trace, cached radiance by roughness and distance).
  See the decision below; `select_reflection()` is still called by nothing that draws.
- Underwater rendering, the projected and traced caustic tiers, caustics seen from below.
- The persistent advected foam field. `water::FoamField` exists and is tested; the shoreline foam
  here is an instantaneous function of the column depth, which is what "foam coverage" in the
  closure needs and not what the `Foam` requirement asks of the simulation.
- `cy/frame.slang`, `src/rendering/**` and the render graph: untouched.

## Decisions

### Two pictures before the frame, drawn by world.slang

The water needs to know what is under it and what is over it. The frame's opaque pass cannot give
either to its own water run: the colour it is writing is not readable in the same pass, and the
assembly's transparent stage has an opaque-colour copy but no depth copy, and cannot be scheduled
after a pass this program adds (the graph schedules in declaration order, and the assembly declares
its stages in one call). So `Stage::declare_water` declares two passes BEFORE the assembly:

- `world water refraction` — the terrain run alone, with the frame's own matrix, into a colour and a
  depth target. Under a water pixel it holds the bed, and the depth reconstructs where.
- `world water reflection` — the sky, terrain, star and foliage runs with `mirrored_rows()`, into a
  colour and a depth target.

Both are drawn with world.slang's own pipeline from copies of the frame's runs, so the reflected hill
is lit, cloud-shadowed and tonemapped exactly as the hill is. The opaque pass then draws the water
run with water.slang and declares the three textures as `FragmentSampledRead`, which is how the graph
orders the passes and transitions the targets; no barrier is written by hand.

The cost is one extra terrain draw and one extra draw of everything above the water.

### Planar, not screen-space

The sea in this world is one body at one mean level, which is exactly the case a planar reflection
is exact for. A screen-space trace needs the frame's depth, which the frame cannot expose to this
program without an assembly change, and it cannot reflect what is off the top of the screen — the
hilltops and the sky above the picture, which from this orbit is most of what the fjord reflects.
The mirrored picture needs no fallback: the sky dome is in it, so wherever no hill is reflected the
sky already is, at the same radiance the dome is drawn with. The trade is the hierarchy the
specification names (screen first, escalating by roughness and distance, cached radiance far away),
which this does not build; the coverage map records it.

### The mirror and its near plane

`mirrored_rows()` applies y -> 2h - y before the frame's matrix. Mirrored, whatever lay under the
water lands above it, between the eye and the surface — the bed would occlude its own reflection. So
the third row is replaced by `row3 - k (y - h)`: reversed-Z depth becomes `1 - k (y - h) / w`, a
vertex whose unmirrored height is below the water gets a depth above one and is clipped, and along
any eye ray the depth still falls monotonically with distance, so the depth test orders the mirrored
world. `k = 0.25` keeps the far side (`k (y - h) <= w`) for everything inside the sky dome. No shader
change and no clip distance were needed, so world.slang and its generated headers are untouched.

### Radiance, not display values

world.slang tonemaps before it stores, so both pictures hold display values. water.slang takes every
read back through the exact inverse of that curve before any light is added, and tonemaps its result
the same way on the way out. Absorption is a multiplication on radiance; on display values it would
be wrong by the curve.

### The column, and src/water/'s own arithmetic

Per pixel: the path is the distance from the surface fragment to the bed point reconstructed from the
refraction depth (along the ray bent by the wave normal, unless the bent read lands on a bank in
front of the water). The refracted radiance is `bed * T + (1 - T) * inScatter * light` with
`T = exp(-(a + b) L)`. `build_water_params()` fills `a + b` from the body's `WaterOptics` and
`inScatter` from `water_in_scatter(optics, infinity)`, so `(1 - T) * inScatter` is
`water_in_scatter(optics, L)` term for term. The column's light is the ambient term plus the sun on a
level surface, cloud-shadowed. Fresnel is Schlick's with `build_closure()`'s F0.

### Shoreline foam

The column under a pixel — the surface's height minus the bed's, from the straight read — against a
declared band of 1.2 m: coverage rises linearly to the shore and is broken up by two octaves of
drifting value noise that never fall below half. Over a column deeper than the band the term is
exactly zero, and the pixel is the pixel without foam. The open sea's existing foam (the device foam
pass's colour) is kept as it was.

### Surface-derived caustics

A refracted sun ray leaving a surface of slope grad(h) is displaced by `D (1 - 1/n) grad(h)` at depth
D, so the bed's area element scales by the Jacobian `1 + D (1 - 1/n) laplacian(h)` and the light on
it by the reciprocal, clamped to four times the unfocused sun. The Laplacian is the analytic one of
the ocean's own trains — `select_caustic_trains()` keeps the sixteen with the largest `a k^2` from
`resolve_trains(model, All)` — with each train's phase at the world-relative origin and at this
frame's water time reduced to one turn on the processor in f64, the way `evaluate_trains` keeps its
own phase exact. Only the sun's share of the bed's light is focused, weakened by
`exp(-(a + b)_green D)` — `underwater_state()`'s caustic strength.

The trains that curve the surface most are the shortest, and a train shorter than two pixels of bed
cannot be drawn, only aliased: the first take showed moire rings following the depth contours of
every distant shallow. So each train is weighted by `saturate(2 - k f 2/pi)`, where `f` is the pixel's
footprint on the bed from the derivatives of the reconstructed bed position (taken in uniform control
flow, before the branch that uses them): full strength below a quarter wavelength per pixel, gone at
the Nyquist limit. `render.world_water` compares only pixels whose whole 3x3 neighbourhood resolves
every train, where the filter is exactly one.

### Off is exact

With water shading off, `Stage::shoot` declares no extra pass and the water run is drawn by the same
pipeline, with the same push block and the same set as before; `record_draw`'s binding sequence is
unchanged. The water objects are created at open either way and never touched when off.

## Backends

- **Vulkan** — the target on this host. `render.world_water` runs the shipped SPIR-V.
- **Metal** — `water_msl.h` is generated from the same source; set 0 at `[[buffer(0)]]`, set 1 (three
  textures and the parameter buffer, an argument buffer as bloom's pass set is) at `[[buffer(1)]]`,
  the push block at `[[buffer(2)]]` in both stages. Compiled, not run, on this host.
- **D3D12** — `samples/10-world` builds no D3D12 path (see `add-cloud-shadows`). `water.slang`
  compiles for DXIL under `just build-shaders --strict`.

## Risks / Trade-offs

- **Two extra geometry passes.** About 1.8 ms of `stage_submit_ms` at the median on a loaded Linux
  host (`evidence/frame-identity.txt`).
- **Refraction at a bank.** A bent read that lands on terrain in front of the water falls back to the
  straight read; a bent read landing on a plant cannot be told apart, because the refraction picture
  holds no plants. The bend is at most 5% of the picture's height per unit slope and the sea's slopes
  are small.
- **fp16 depth of the reflection.** None: the mirrored depth is a D32 target.
