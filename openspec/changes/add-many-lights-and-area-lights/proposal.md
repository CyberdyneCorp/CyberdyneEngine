# Add many lights and area lights

## Why

`rendering-lighting-and-shadows` asks for six light types with a mobility classification, and for a
stochastic many-light path. The requirements map recorded both as `exempt:m11e`. "Light types":
"MOBILITY DOES NOT EXIST … and the six light types are three" — `Rect`, `Disc` and `Tube` existed
only as LTC shapes in `area.h`, never as a light a frame could carry. "Stochastic many-light direct
lighting": "THE ESTIMATOR IS OBSERVED, THE PATH IS NOT RUN" — the reservoir algebra was checked
against an exact sum on the host, and nothing drew a frame with it, so the third scenario (the same
content under both paths) had nothing to compare.

A game also had no way to make a light for a moment — a muzzle flash, a spark, an explosion's
fireball — without authoring an entity for it.

## What changes

- **Area lights reach the frame.** `render::LightKind` appends `Rect`, `Disc`, `Sphere` and `Tube`
  (luminance in nits, `LightDescription::source_width`, `source_height`, `two_sided`). `GpuLight`
  carries them in the existing 64-byte record (half extents, normal, an octahedral tangent);
  `cy/light.slang` shades them with linearly transformed cosines — the diffuse term as the polygon's
  form factor, the specular term as the LTC integral with Fresnel taken at the lobe's gather
  direction rather than the hemisphere's split sum. A sphere is the disc it subtends; a tube is its
  silhouette rectangle. The 24×24 LTC table (`LtcTable::build`) is uploaded once as a bindless
  texture by `lights_gpu::LtcTableTexture` and named in the view block; without it an area light
  shades its diffuse term only. The host twin is `lighting/area_shading.h`.
- **Mobility.** `render::LightMobility` (`Static`, `Stationary`, `Movable`, the default). A static
  light is baked whole: the assembly gives it a placeholder record no loop shades, in no cluster
  list and no candidate set. `AssemblyReport` counts `static_lights` and `area_lights`.
- **The GPU light list.** `cy::rendering-lights-gpu` (`src/rendering/lights_gpu/`), a new module:
  `GpuLightAssignment` culls every light against each tile's frustum and keeps, per depth slice, the
  nearest lights (at most 64) whose sphere reaches it, on the device, from a device-local copy of the
  light records; the frame reads its lists. `FramePassKind` appends `ManyLights`; the frame
  description gains the cluster-assignment and many-lights stages, declared through
  `FrameStageDeclaration`.
- **The stochastic path.** `ManyLightsPass`: per pixel, light candidates drawn from an alias table in
  proportion to power, resampled (RIS) into a reservoir; temporal reuse with a capped history;
  spatial reuse over neighbours with generalized balance-heuristic weights (unbiased); one
  screen-space visibility ray for the selected sample; and the denoiser's `StochasticDirect` signal
  (`accumulate`, variance, à-trous) transcribed. `LightingPathSelector` chooses the path from the
  frame arbiter's shading allocation and the cluster bound's drops, leaving at once and returning
  after a dwell; the path is refused without a denoiser.
- **Lights for a moment.** `rendering::TransientLights`: a fixed pool of point, spot and area lights
  with a lifetime and a fade, generational handles, no allocation per spawn; the live lights are
  appended to the frame's lights. ABI 1.11 appends `light_spawn`, `light_update`, `light_destroy`,
  `light_alive` and `light_status` over it (`cy::game-backend-lights`), refused in a fixed update;
  CyberdyneKit gains `Light`.
- **Area lights in CyberGI.** `gi_gpu::append_gi_lights` hands an area light to the GI as the
  emitter squares that cover its surface, radiating its flux; the editor's GI preview uses it.
- **The editor.** `scene.create-light` makes rect, disc, sphere and tube lights (a size in metres,
  intensity in nits), from the menu, the hierarchy, scripts and MCP; `LightSource` appends
  `source_width`, `source_height`, `two_sided` and `mobility`, extracted into the frame and hashed as
  authoritative state; the editor's frame reads the area fields. An area light writes no
  `.cylightmap` line yet rather than baking as a point.
- One defect found on the way, with a regression test: capping a reservoir's sample count without
  scaling its weight sum changed its contribution weight (`cap_reservoir`).

## Scope

Visibility for the stochastic path is one screen-space ray against the depth buffer: occluders off
screen do not shadow, and the ray-traced variant is left to the tracer. The lightmap baker does not
take area lights. Nothing in the samples binds the lights backend to a running game yet; the
first-person weapon demo is its first consumer. The device paths run on Vulkan; Metal and D3D12
compile every shader (`build-shaders --strict`) and were not run.
