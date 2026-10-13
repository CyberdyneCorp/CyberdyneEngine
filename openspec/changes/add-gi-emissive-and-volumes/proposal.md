# Classify and promote emissive surfaces, and place GI volumes

## Why

Two requirements of `rendering-global-illumination` were exempt in
`tools/roadmap/requirements-coverage.toml` because nothing in the tree implemented them:

- **Emissive surfaces as illumination.** An emissive card already fed the bounce through
  `SurfaceCache::outgoing`, but nothing classified emitters by radiant power and area, nothing
  promoted a small bright emitter to a light, and no case measured an emitter lighting a room. A
  card smaller than the cache's lookup cell is worse than noisy in the cache: every gather hit in
  the cell resolves to it, so its energy is inflated by the cell's area over its own and the
  receivers below it are a field of fireflies.
- **GI volumes.** `IlluminationSettings` was global. There was no authored region overriding
  quality, probe density, update rate, intensity, tier preference or the far-field distance, and no
  priority or blend distance between overlapping ones.

## What changes

- **`gi/emissive.h`.** `classify_emitters` groups emissive cards by (instance, material) and
  classifies each emitter by radiant power (`pi * Y(L) * A`) and area. An emitter at least
  `promotion_power` strong and at most `promotion_max_area_m2` large is PROMOTED: its emission
  leaves the cache and each of its cards becomes a `GiLight` with `emitter_area` and
  `emitter_normal`. Everything else stays CACHE ONLY. A promoted emitter over the cell's light budget
  is DEMOTED back to the cache. `EmissiveReport` accounts for every watt (`total = cached + promoted
  + demoted`, `clamped = 0`). `EmissiveLights` keeps the classification per resident cell.
- **The direct lighting path integrates a promoted card in closed form.** `direct_radiance` and the
  device's `giDirectRadiance` evaluate `emitter_form_factor` — Lambert's point-to-polygon form factor
  over the card clipped to the receiver's horizon, angles by `atan2` — and `shadow_target` stops the
  shadow ray `kEmitterShadowLiftMetres` in front of the emitter. Every consumer of `shaded_direct`
  (the surface cache, the device card shading, the path tracer's next-event term, the irradiance
  volume capture) gets it. The device uses the light record's fourth `float4`, which was zero; it
  stays zero for every other light, so a scene with no promoted emitter uploads the same bytes.
- **`IlluminationSystem::ingest_cell` / `evict_cell`** classify a cell, ingest the cache surfels,
  allocate pages, and file a `LightChanged` invalidation attributed to each promoted light over the
  region it lights. `update()` shades with the frame's lights plus the promoted ones
  (`frame_lights()`), and with exactly the frame's span when nothing is promoted.
- **`PathTracer::set_emission_footprints`.** A reference over cards takes a card's emission only
  where a hit lies inside the card's own square; off by default, which is every existing bake.
- **`gi/gi_volume.h`.** `GiVolume` (bounds, priority, blend distance, weight, and the parameters it
  sets), `GiVolumeSet::resolve` (per parameter, lowest priority first, each lerping toward its value
  by `weight * fade`; the discrete tier switches at weight one half) and `validate_gi_volume`.
- **What consumes a volume.** Probe placement (density above one: full density; below: the sparse
  lattice only), each probe's recorded `update_rate`, `ray_scale` and `tier`, the scheduler (a probe
  ages at its rate, and is starved at `max_age / rate`), the probe gather (rays scaled, the software
  tracer for `SoftwareOnly`), `TieredTracer::trace` with a preference (`SoftwareOnly`, `WorldOnly`),
  and `indirect_diffuse` / `indirect_specular` (intensity and the far-field distance, per sample).
  `IlluminationSystem::set_volumes` retires the probes in the reach of every volume that sets a
  placement parameter, so the next update places them again.
- **Tests.** `integration.render_gi_emissive`, `integration.render_gi_volumes`, the resolution cases
  in `unit.render_gi`, and a device case in `render.gi_gpu`.

## Scope

The GI system's own direct lighting path. Not in this change: handing promoted emitters to the
forward frame's clustered lights or the stochastic many-light path (no runtime owner of
`IlluminationSystem` exists yet — issue #35 stage 4), Swift and editor surfaces for GI volumes, and
volumes affecting the surface cache's page rate (the probes carry the rate).
