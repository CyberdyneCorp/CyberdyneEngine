# rendering-global-illumination Specification

## Purpose

Defines **CyberGI**: fully dynamic diffuse and specular indirect illumination for large worlds,
resolved by combining screen-space visibility, adaptive world-space radiance caching, software
scene tracing, and hardware ray tracing under one quality and GPU-time budget.

The organising rule is **use the cheapest source that can give a trustworthy answer** — where
trustworthy is a confidence value the system computes, not a fixed fallback order. Screen tracing
answers most rays almost free and cannot answer any ray that leaves the screen; world tracing
answers the rest; confidence decides which is used and whether spending more is worthwhile.

Three decisions make it work. A named **GI scene** defines what the world looks like to an
illumination query, at a deliberately coarser error target than primary visibility — centimetres
rather than sub-pixels — drawn from the virtual geometry hierarchy rather than from a second
simplification. A **surface cache** turns a traced hit into a lookup rather than a material
evaluation, which is what makes hybrid tracing affordable at all, and which incidentally provides
multi-bounce: radiance fed back into the gather approximates additional bounces over frames.
And **diffuse GI and reflections are one system** with two ray distributions, sharing the scene,
the caches, the tracer, the denoiser, and the budget.

Baked lighting is not a legacy path. It remains a first-class mode for constrained hardware, and it
seeds the dynamic caches so a level is plausible on its first frame instead of converging from
black. The offline path tracer that produces it is the same one that produces ground-truth
references for measuring how wrong the real-time result is.

Direct lighting stays out of this system deliberately — many shadowed lights is a direct-lighting
problem, and solving it here would produce two lighting models that disagree at their boundary.

## Requirements
### Requirement: Engine-owned illumination architecture
The illumination system SHALL be engine code: the GI scene and its representations, the surface
cache, the radiance cache and its scheduler, the tracing tiers and their selection, the resolve,
the budget policy, and the diagnostics.

It SHALL be decomposed into named subsystems with separate ownership — GI scene, surface cache,
distance field, radiance cache, tracers (screen, software, hardware), resolve, reflections,
denoising, baking, and diagnostics — rather than a single illumination module.

Hardware ray tracing SHALL be consumed through `ray-tracing-infrastructure` as **one tracing tier
within one renderer**, not as an alternative renderer. There SHALL NOT be a separate ray tracing
rendering path.

#### Scenario: Ray tracing is a capability, not a renderer
- **WHEN** hardware ray tracing is available
- **THEN** it SHALL be used as a tracing tier by GI, reflections, shadows, and ambient occlusion,
  and no parallel renderer SHALL exist

#### Scenario: Subsystems are separable
- **WHEN** the radiance cache is replaced or reworked
- **THEN** the GI scene, surface cache, tracers, and resolve SHALL be unaffected

### Requirement: GI strategy layers
The engine SHALL support **GI modes**, selectable per project and per platform profile:

| Mode | Behaviour |
|---|---|
| `None` | Ambient and sky only |
| `Baked` | Lightmaps and baked probes |
| `Probe` | Baked probes for dynamic objects, no lightmaps |
| `Dynamic` | Fully dynamic hybrid illumination |
| `Hybrid` | Dynamic illumination seeded and supplemented by baked data |

Within a mode, indirect radiance SHALL be drawn from these **sources**, combined by confidence:

| Source | Covers | Cost |
|---|---|---|
| Lightmaps | Static geometry lit by static lights | Bake time; near-zero runtime |
| Irradiance volumes | Dynamic objects in baked environments | Small runtime |
| Radiance cache | Dynamic diffuse and rough specular at all scales | Moderate runtime |
| Surface cache | Radiance at traced hits | Moderate runtime |
| Screen tracing | Contact-accurate detail where visible | Small runtime |
| Reflection probes | Localised specular fallback | Small runtime; capture cost |

Sources SHALL be combined without double counting: a surface taking a bounce from one source SHALL
NOT accumulate the same bounce from another.

Baked data SHALL remain a first-class mode, not a compatibility path: it is the correct answer for
constrained hardware, and it **seeds** the dynamic caches so a level is plausible on its first
frame.

#### Scenario: No double counting
- **WHEN** a lightmapped surface is inside a dynamic GI region
- **THEN** it SHALL take indirect diffuse from the lightmap only, and the dynamic contribution
  SHALL be excluded for it

#### Scenario: Fallback chain for specular
- **WHEN** a screen-space specular ray fails
- **THEN** the result SHALL fall back through the world tracer and radiance cache to the nearest
  reflection probe and then the sky, blended by confidence rather than switching abruptly

#### Scenario: Mode is a profile decision
- **WHEN** a project targets both high-end desktop and mobile
- **THEN** it SHALL select `Dynamic` or `Hybrid` for one profile and `Baked` for the other, with
  the same content

### Requirement: The GI scene
The engine SHALL maintain a **GI scene**: the representation of the world used to answer
illumination queries, derived from the GPU scene and maintained incrementally.

The GI scene SHALL be **deliberately coarser than primary visibility**, with its own error
targets:

| Query | Target error |
|---|---|
| Primary visibility | Sub-pixel geometric error |
| Near-field illumination | Centimetres of world-space error |
| Far-field illumination | Metres of world-space error |

Where an asset has a virtual geometry hierarchy, the GI scene SHALL select a coarser level from
that hierarchy rather than building a separate simplification.

The GI scene SHALL be **cell-scoped**: illumination payloads SHALL be cooked as a cell channel and
ingested on cell residency, and evicted on cell unload, so no monolithic global structure exists.
Cell lifecycle SHALL come from `world-partition-and-streaming`.

Ingestion and eviction SHALL be incremental and SHALL invalidate only the affected region, so
streaming does not cause a global illumination rebuild.

#### Scenario: Illumination does not pay for pixel-accurate geometry
- **WHEN** a ray is traced for indirect diffuse
- **THEN** it SHALL intersect a representation at the illumination error target, not the primary
  visibility representation

#### Scenario: One hierarchy, two error targets
- **WHEN** an asset has a virtual geometry hierarchy
- **THEN** the GI scene SHALL request a coarser level from it, rather than cooking a second
  simplified mesh

#### Scenario: Region unloads
- **WHEN** a world cell is unloaded
- **THEN** its GI payloads SHALL be evicted, the affected region invalidated, and queries into it
  SHALL fall back to the far-field representation

#### Scenario: Streaming does not rebuild illumination
- **WHEN** cells stream in and out as the camera moves
- **THEN** GI scene updates SHALL be incremental and local, not a global rebuild

### Requirement: Surface cache
The engine SHALL maintain a **surface cache**: shaded radiance for world surfaces, stored per
surface parameterisation, holding at minimum position, normal, albedo, roughness, emission, direct
lighting, and accumulated radiance.

A traced hit SHALL resolve to a **cache lookup**, not to a material evaluation. Evaluating a full
material graph at a secondary hit SHALL NOT be required.

The cache SHALL be filled by the material's **secondary program** (see `material-compiler`), not
its primary program.

Accumulated radiance SHALL include previously gathered indirect light, so feeding the cache back
into the gather produces a **multi-bounce approximation** over frames without tracing long paths.

Cache entries SHALL carry an age and a validity state, and SHALL be updated under a budget with
priority given to visible, recently invalidated, and high-error regions.

#### Scenario: A secondary hit is a lookup
- **WHEN** a GI ray hits a surface
- **THEN** it SHALL read cached radiance, and SHALL NOT decode geometry, sample the material's
  texture set, and evaluate its closures

#### Scenario: Bounces accumulate over frames
- **WHEN** cached radiance is fed back into the gather
- **THEN** successive frames SHALL approximate additional bounces, converging toward a
  multi-bounce solution

#### Scenario: Colour bleeding comes from the cache
- **WHEN** a saturated red wall is lit
- **THEN** the cache SHALL record its reflectance, and nearby surfaces SHALL receive tinted
  indirect light

### Requirement: Distance field representation
The engine SHALL maintain a **sparse signed distance field** of the world in camera-centred
clipmaps of increasing extent and decreasing resolution, used for software sphere tracing, coarse
occlusion, and sky visibility.

Clipmaps SHALL scroll with the camera, re-solving only newly exposed regions.

The representation SHALL be sparse: regions containing no surface SHALL not consume storage.

Per-asset distance fields SHALL be generated at cook time and composited into the world field,
so a moving object updates the field by transform rather than by regeneration.

#### Scenario: Kilometre-scale coverage
- **WHEN** the camera is in a large open world
- **THEN** clipmap levels SHALL cover from metres to kilometres, with resolution decreasing with
  extent

#### Scenario: Camera translation is incremental
- **WHEN** the camera moves
- **THEN** clipmaps SHALL scroll and only newly exposed regions SHALL be re-solved

#### Scenario: Sky visibility from the field
- **WHEN** a surface is indoors
- **THEN** tracing the field toward the sky SHALL report occlusion, and the surface SHALL not
  receive full outdoor irradiance

### Requirement: Radiance cache
The engine SHALL maintain a **radiance cache** of world-space probes storing incoming radiance,
queried by many pixels rather than solved per pixel.

Probe placement SHALL be **adaptive and geometry-aware**, not a uniform grid: density SHALL be
higher near surfaces, at corners, at doorways, at lighting transitions, and in the player's
region, and lower in open space and at distance. Probes inside solid geometry SHALL not be
allocated.

Probes SHALL be organised in **camera-centred clipmaps** so large worlds are covered at decreasing
density, scrolling with the camera and reusing probes that remain valid.

Probe encoding SHALL be selectable — spherical harmonics, octahedral directional maps, or
spherical Gaussians — with the trade-off between memory, directionality, and evaluation cost
documented.

Probes SHALL store a **visibility** term so a probe on the far side of a wall does not leak light
into a query.

#### Scenario: Probes are not wasted on emptiness
- **WHEN** probes are placed in a large open area
- **THEN** density SHALL be low there and high near surfaces, rather than uniform throughout

#### Scenario: Camera moves through a large world
- **WHEN** the camera translates
- **THEN** clipmaps SHALL scroll, valid probes SHALL be reused, and only newly exposed regions
  SHALL be populated

#### Scenario: No leaking through walls
- **WHEN** a query interpolates probes across a wall
- **THEN** the visibility term SHALL suppress the contribution of probes not visible from the
  query point

### Requirement: Probe update scheduling
Probe updates SHALL be **budgeted and prioritised**, not performed for every probe every frame.

Priority SHALL combine: whether the probe affects visible surfaces, distance from the camera,
recency of invalidation, age since last update, estimated error, and the importance of the region
it serves.

The number of probes updated per frame SHALL follow from the GI budget allocation, and the
scheduler SHALL report the queue depth and the oldest unserviced probe.

The scheduler SHALL guarantee **progress**: no valid probe shall be starved indefinitely by
higher-priority work.

#### Scenario: Large cache, bounded cost
- **WHEN** two hundred thousand probes exist and the budget permits four thousand updates
- **THEN** four thousand SHALL be updated, selected by priority, and the remainder SHALL retain
  their previous values

#### Scenario: Visible and stale is serviced first
- **WHEN** a probe affecting visible surfaces has just been invalidated
- **THEN** it SHALL be prioritised over a distant probe of similar age

#### Scenario: No starvation
- **WHEN** a low-priority region is never visible
- **THEN** its probes SHALL still be refreshed at a bounded minimum rate

### Requirement: Incremental invalidation
Changes to geometry, lighting, or materials SHALL invalidate **only the affected region** of the
illumination state.

The system SHALL detect changes from the GPU scene and map them to: surface cache pages, distance
field bricks, acceleration structure updates, and probes within the affected radius, enqueuing
each for prioritised update.

A global rebuild SHALL NOT be required for a local change.

Invalidation SHALL be reportable, so a developer can see what a change caused.

#### Scenario: A door opens
- **WHEN** a door moves
- **THEN** the distance field bricks, surface cache pages, and probes near it SHALL be invalidated
  and re-solved, and the rest of the world SHALL be untouched

#### Scenario: A light changes colour
- **WHEN** a light switches from red to blue
- **THEN** surfaces within its influence SHALL be re-shaded into the cache and nearby probes
  invalidated, converging over the following frames

#### Scenario: Invalidation is attributable
- **WHEN** GI cost spikes
- **THEN** the system SHALL report which changes caused which invalidations

### Requirement: Screen-space tracing
The engine SHALL provide **screen-space tracing** as the first and cheapest tier for both diffuse
and specular queries, marching the depth buffer using a hierarchical depth pyramid.

Screen tracing SHALL be understood as an **optimisation, not a solution**: it cannot answer rays
that leave the screen, reach surfaces hidden behind visible geometry, or intersect thin geometry
reliably.

Every screen-space result SHALL carry a **confidence** derived from hit validity, screen-edge
proximity, and thickness agreement, so the resolve blends rather than switches, and so the tracer
can decide whether a more expensive tier is warranted.

Resolve SHALL be roughness-dependent: broader lobes SHALL widen the filter and reduce ray count.

#### Scenario: Ray leaves the screen
- **WHEN** a traced ray exits the viewport
- **THEN** confidence SHALL fall toward zero near the edge and a higher tier or cached source
  SHALL take over smoothly

#### Scenario: Rough surface
- **WHEN** the surface is rough
- **THEN** the resolve SHALL widen its filter and reduce ray count, since the lobe is broad

#### Scenario: Hidden surface is not invented
- **WHEN** a ray would hit a surface not present in the depth buffer
- **THEN** the screen tier SHALL report low confidence rather than returning a plausible-looking
  wrong answer

### Requirement: Tracing tiers and selection
Illumination queries SHALL be answered by tiers of increasing cost — **screen**, **software**
(distance field and surface cards), and **hardware** (ray queries through
`ray-tracing-infrastructure`) — behind one interface.

The system SHALL use **the cheapest tier that can give a trustworthy answer**, escalating when
confidence is below a threshold and the budget permits.

Consumers SHALL request radiance along a ray and SHALL NOT branch on which tier answered, nor on
device capability.

Tier availability SHALL be a capability and profile decision. With no hardware tier available, the
system SHALL remain fully functional through the software tier.

#### Scenario: Cheapest sufficient answer
- **WHEN** a screen trace hits with high confidence
- **THEN** no world trace SHALL be issued for that sample

#### Scenario: Escalation on low confidence
- **WHEN** a screen trace has low confidence and budget remains
- **THEN** a software or hardware trace SHALL be issued for that sample

#### Scenario: Software-only device
- **WHEN** no hardware ray tracing is available
- **THEN** dynamic GI SHALL still function through screen and software tiers, with quality and
  cost differences documented

### Requirement: Sample confidence
Every radiance sample SHALL carry a **confidence** value expressing how much the resolve should
trust it, derived from its source, hit validity, cache age, invalidation state, and temporal
history validity.

Resolve SHALL combine sources weighted by confidence rather than selecting one, so transitions
between sources are not visible.

Confidence SHALL be exposed to diagnostics, because "which source answered this pixel and how much
was it trusted" is a question the system must be able to answer.

#### Scenario: Blending, not switching
- **WHEN** a pixel's screen-space confidence falls as the camera turns
- **THEN** its radiance SHALL transition smoothly toward the world-traced or cached source

#### Scenario: Stale cache is distrusted
- **WHEN** a region has been invalidated and not yet re-solved
- **THEN** samples from it SHALL carry reduced confidence and the resolve SHALL weight them down

### Requirement: Dynamic diffuse global illumination
Dynamic diffuse indirect lighting SHALL be resolved from the radiance cache, the surface cache,
and the tracing tiers, combined by confidence and accumulated temporally through the framework in
`temporal-rendering`.

Multi-bounce SHALL be approximated by feeding previously accumulated radiance back into the
gather, giving an infinite-bounce approximation over frames without tracing long paths.

Rays SHALL be distributed across frames rather than resolved fully per frame: a probe or pixel MAY
gather a subset of directions each frame, accumulating toward a stable result.

The system SHALL function without hardware ray tracing, using the screen and software tiers, with
the quality and cost differences documented.

#### Scenario: Camera moves
- **WHEN** the camera translates by one probe spacing
- **THEN** the radiance clipmap SHALL scroll and only the newly entered region SHALL be re-solved

#### Scenario: Lighting change converges
- **WHEN** a light is switched on
- **THEN** the affected region SHALL be invalidated and converge over a bounded number of frames,
  with the convergence rate exposed as a quality setting and the unconverged state reportable

#### Scenario: Ray tracing unavailable
- **WHEN** the device lacks hardware ray tracing
- **THEN** the software tier SHALL be used with the same caches, resolve, and sampling code

### Requirement: Reflections share the illumination infrastructure
Specular indirect lighting SHALL use the same GI scene, caches, tracing tiers, resolve, and
denoiser as diffuse indirect lighting, differing only in ray distribution and roughness handling.

Strategy SHALL be selected by roughness:

| Roughness | Strategy |
|---|---|
| Near-mirror | Dedicated traced rays at the highest available tier |
| Moderate | Sparse traced rays combined with the radiance cache |
| Rough | Radiance cache only, with no dedicated reflection rays |

Thresholds SHALL be configurable per quality tier.

There SHALL NOT be a separate reflection scene representation, reflection cache, or reflection
denoiser.

#### Scenario: Rough reflections cost nothing extra
- **WHEN** a surface is very rough
- **THEN** its reflection SHALL be taken from the radiance cache without dedicated rays

#### Scenario: Mirror gets real rays
- **WHEN** a surface is near-mirror
- **THEN** dedicated rays SHALL be traced at the highest available tier within budget

#### Scenario: One infrastructure
- **WHEN** reflections and diffuse GI are both active
- **THEN** they SHALL share the GI scene, caches, and denoiser rather than duplicating them

### Requirement: Reflection probes
Reflection probes SHALL be **one source within the illumination hierarchy**, not a separate
reflection system: they are consulted when higher tiers report low confidence, and they remain the
primary specular source in `Baked` and `Probe` modes and on constrained profiles.

Probes SHALL capture the surrounding scene into a roughness-filtered octahedral radiance map,
with:

- capture modes: **baked** (once, offline), **on demand**, and **realtime** (amortised across
  frames)
- **box** and **sphere** influence volumes with a blend distance
- **box projection** (parallax correction) so reflections align with room geometry
- an importance value resolving overlapping probes
- an interior flag excluding the sky

Probes SHALL be assigned to fragments through cluster assignment and blended by influence weight.

Realtime probe capture SHALL draw from the GI allocation like any other illumination work.

#### Scenario: Parallax-corrected reflection
- **WHEN** box projection is enabled
- **THEN** the reflection vector SHALL be intersected with the probe's box and re-aimed at the
  hit point, so a wall reflects at the right place

#### Scenario: Realtime probe is amortised
- **WHEN** a realtime probe updates
- **THEN** its faces and filtering mips SHALL be spread across several frames under the GI budget

#### Scenario: Probe as a confidence fallback
- **WHEN** a specular query cannot be answered by tracing
- **THEN** the probe result SHALL be blended in by confidence rather than switched to

### Requirement: Ray-traced effects
Hardware ray tracing SHALL be a **tracing tier** available to illumination queries, reflections,
soft shadows, and ambient occlusion — not a separate rendering mode and not a requirement.

Ray-traced features SHALL consume the **ray tracing infrastructure** (see
`ray-tracing-infrastructure`) for acceleration structures, geometry adapters, and ray queries.
They SHALL NOT build, refit, or own acceleration structures themselves.

A hardware hit SHALL resolve through the **surface cache**, not through full material evaluation,
so ray cost does not scale with material complexity.

Ray-traced features SHALL be capability-gated and each SHALL have a non-ray-traced path, so no
content depends on their presence. Traced ray counts SHALL be part of the GI allocation.

#### Scenario: Ray-traced reflection beyond the screen
- **WHEN** ray tracing is available and screen-trace confidence is low
- **THEN** a traced ray SHALL supply the reflection instead of falling back to a probe

#### Scenario: Acceleration structure maintenance
- **WHEN** dynamic geometry moves
- **THEN** the ray tracing infrastructure SHALL refit its bottom-level structures and rebuild the
  top-level structure within its declared budget, and GI SHALL not manage that itself

#### Scenario: Virtual geometry is traced as a proxy
- **WHEN** a traced ray hits virtual geometry
- **THEN** it SHALL intersect the proxy representation, and the documented difference from the
  rasterised surface SHALL apply

#### Scenario: A hardware hit is still a cache lookup
- **WHEN** a hardware ray hits a surface
- **THEN** its radiance SHALL come from the surface cache, not from evaluating the material's
  primary program

### Requirement: Emissive surfaces as illumination
Emissive materials SHALL contribute to indirect illumination through the surface cache, so an
emissive surface lights its surroundings without an explicitly placed light.

Emissive surfaces SHALL be **classified by radiant power and area**:

- below a configured threshold: contributing through the cache only, and excluded from importance
  sampling
- above it: **promoted to a light**, sampled deterministically by the direct lighting path

Small, extremely bright emissive surfaces SHALL NOT be left to stochastic sampling, since that is
the primary source of fireflies. Energy SHALL be redistributed by classification rather than
suppressed by clamping.

#### Scenario: Neon sign lights the room
- **WHEN** an emissive sign is present with no light placed
- **THEN** nearby surfaces SHALL receive indirect illumination from it

#### Scenario: Tiny bright emitter is promoted
- **WHEN** a small surface has very high emissive power
- **THEN** it SHALL be promoted to a light and sampled deterministically, rather than producing
  sparse bright samples

#### Scenario: Energy is not silently clamped
- **WHEN** a bright emitter is handled
- **THEN** the mechanism SHALL be classification and importance sampling, and any clamping applied
  SHALL be reported as energy loss

### Requirement: Sky and atmosphere
Sky and atmospheric models are defined in `atmosphere-sky-and-clouds`: the physically based
participating atmosphere, its precomputed tables, celestial bodies and time of day, cloud
representation and rendering, and aerial perspective.

This capability defines what illumination **consumes** from them: a filtered radiance map for
specular image-based lighting, irradiance for ambient diffuse, sun and celestial transmittance, cloud
shadowing, and aerial perspective applied consistently with volumetric media.

Sky-derived illumination SHALL update when the sky changes, **incrementally where it changes
continuously** — a moving sun, thickening cloud cover — and under the illumination budget rather than
triggering unbounded recomputation.

A change in atmospheric or cloud state SHALL invalidate only the illumination that depends on it:
sky-lit surfaces and the affected regions of the radiance and surface caches.

Volumetric clouds SHALL contribute shadowing through the coarse cloud shadow field, not by
participating in shadow page allocation.

#### Scenario: Time of day
- **WHEN** the sun rotates continuously
- **THEN** the atmosphere's tables SHALL update incrementally and the radiance and irradiance
  illumination consumes SHALL follow without a visible step

#### Scenario: Aerial perspective
- **WHEN** the physical atmosphere is enabled
- **THEN** distant geometry SHALL receive scattering consistent with the sky, not a separately tuned
  fog

#### Scenario: Cloud cover changes the light
- **WHEN** cloud coverage thickens
- **THEN** sky irradiance SHALL fall, and the illumination affected SHALL be updated incrementally
  within budget

### Requirement: Illumination classification of dynamic objects
Every renderable SHALL be classified for illumination purposes: `Static`, `SlowDynamic`,
`FastDynamic`, or `TransientIgnored`, determining its representation in the GI scene and the
invalidation it causes.

| Class | Behaviour |
|---|---|
| `Static` | Full participation; contributes to cache and field; invalidates on change |
| `SlowDynamic` | Participates; invalidation rate-limited and amortised |
| `FastDynamic` | Contributes approximate occlusion and reflection presence; does not invalidate caches per frame |
| `TransientIgnored` | Excluded from the GI scene entirely |

Classification SHALL be derivable from mobility and size, and overridable per object.

Skinned geometry SHALL supply its illumination representation from the GPU pose world, at a
fidelity chosen by importance: a deformed proxy near the camera, a coarse proxy at distance.

#### Scenario: A projectile does not invalidate the world
- **WHEN** a bullet crosses a room
- **THEN** it SHALL be classified as transient and cause no cache or field invalidation

#### Scenario: A vehicle does
- **WHEN** a large vehicle drives through a scene
- **THEN** it SHALL participate in the GI scene, with invalidation amortised rather than issued
  every frame

#### Scenario: Crowd is approximated
- **WHEN** a distant crowd is animated
- **THEN** its members SHALL use coarse proxies rather than deformed illumination geometry

### Requirement: Far-field illumination
Beyond a configured distance, illumination SHALL use a **far-field representation**: coarse
geometry, the lowest distance field clipmap, and a low-frequency radiance cache.

The far field SHALL provide plausible illumination and sky occlusion at kilometre scale at bounded
cost, and SHALL blend with the near field without a visible boundary.

Content SHALL NOT be required to author a far-field representation; it SHALL be derived.

#### Scenario: Distant mountains are lit
- **WHEN** terrain kilometres away is visible
- **THEN** it SHALL receive far-field illumination without tracing detailed geometry

#### Scenario: No visible boundary
- **WHEN** the camera moves across the near-field to far-field transition
- **THEN** illumination SHALL blend rather than step

### Requirement: Transparency and participating media
Illumination support for non-opaque surfaces SHALL be staged, with the classification declared:

| Class | Support |
|---|---|
| Opaque, masked | Full participation |
| Thin transmission | Supported: receives indirect, transmits approximately |
| Volumetric media | Receives low-frequency indirect from the radiance cache |
| True refraction | Deferred; interface reserved |
| Caustics | Deferred; interface reserved |

Volumetric fog (see `rendering-post-processing`) SHALL receive indirect radiance from the radiance
cache rather than computing its own indirect term.

#### Scenario: Foliage transmits
- **WHEN** a leaf with thin transmission is backlit
- **THEN** it SHALL transmit indirect light approximately, without a refraction solve

#### Scenario: Fog is indirectly lit
- **WHEN** fog fills an indirectly lit interior
- **THEN** it SHALL take low-frequency indirect radiance from the radiance cache

#### Scenario: Deferred features are named
- **WHEN** a project needs caustics
- **THEN** the specification SHALL state that they are deferred, rather than leaving the
  expectation open

### Requirement: GI budget and importance
The illumination system SHALL hold the GI allocation issued by the renderer budget arbiter (see
`rendering-architecture`) and distribute it internally across: probe updates, ray counts per tier,
tracing resolution, surface cache update rate, cache density, reflection resolution, and denoiser
quality.

It SHALL measure and report its own cost, and SHALL NOT measure total frame time.

Distribution SHALL be **importance-aware**, combining screen coverage with per-object importance
from the ECS, so quality follows what matters in the game rather than distance alone.

The system SHALL declare a reserved minimum and report when it has reached it.

Adjustment SHALL be smooth and hysteretic, on a shorter time constant than the arbiter's
reallocation. **Pinned mode** SHALL be global with the arbiter.

Where a foveation mask is supplied, sample density SHALL follow it.

#### Scenario: Budget is held by internal redistribution
- **WHEN** measured GI cost exceeds its allocation
- **THEN** the system SHALL reduce probe updates, ray counts, and resolution in a declared order,
  reporting each lever applied

#### Scenario: Gameplay importance drives quality
- **WHEN** a gameplay-critical unit and background scenery are both visible
- **THEN** the unit SHALL receive higher probe density and reflection quality within the same
  allocation

#### Scenario: Quality does not oscillate
- **WHEN** load hovers around the allocation
- **THEN** hysteresis SHALL prevent visible pumping of GI quality

### Requirement: GI volumes
Artists SHALL be able to place **GI volumes** overriding, within their bounds: quality level,
probe density, update rate, indirect diffuse and specular intensity, tier preference, and the
far-field transition distance.

Volumes SHALL have priority and blend distance, resolving overlap by the same rules as
post-process volumes.

Volumes SHALL be an override, not a requirement: a scene SHALL be correctly lit with none placed.

#### Scenario: Important interior
- **WHEN** a designer marks an interior as high quality
- **THEN** probe density and update rate SHALL increase there within the GI allocation

#### Scenario: No volumes needed
- **WHEN** a scene contains no GI volumes
- **THEN** it SHALL be lit correctly using global settings

### Requirement: Convergence and capture
The system SHALL expose a **convergence metric** per region: how far the current illumination
state is from its converged value.

A **converged mode** SHALL be provided: the scheduler is forced to full update rates and frames are
advanced until the convergence metric falls below a threshold or a frame cap is reached, after
which the result is stable and reproducible.

Converged mode SHALL be used by golden-image tests, cinematic capture, and reference comparison,
so temporal convergence does not make rendering tests unreliable.

Convergence SHALL be queryable at runtime, so "this lighting change has not finished converging"
is a reportable state rather than a suspected one.

#### Scenario: Reproducible test capture
- **WHEN** a golden-image test renders a GI scene
- **THEN** converged mode SHALL be used, and repeated runs SHALL produce the same image

#### Scenario: Convergence is observable
- **WHEN** a light changes and the image is still settling
- **THEN** the convergence metric SHALL report the affected regions as unconverged

### Requirement: Lightmap baking
The engine SHALL bake static illumination using the **offline path tracer**, sharing its scene,
material, and light representations with the real-time path.

The bake SHALL: gather static meshes and their UV2 charts, build an acceleration structure,
path-trace direct and indirect lighting for a configurable bounce count, denoise the result,
dilate and pad chart borders, and pack into atlases.

Bake outputs SHALL be selectable: **irradiance only** (single texture), **directional** (an
irradiance plus a dominant direction, preserving normal-map response), or **spherical harmonics**
(L1, preserving more directionality at higher memory cost).

The bake SHALL additionally produce **seeds for the dynamic caches** — initial surface cache and
radiance cache contents — so a level in `Hybrid` mode is plausible before convergence.

The bake SHALL support: per-object lightmap resolution scaling, a global texel density, emissive
surfaces as light sources, transparent and alpha-tested occlusion, and a **shadow mask** allowing
stationary lights to keep dynamic direct light with baked shadows.

Baking SHALL be **incremental** where possible: unchanged geometry and lighting SHALL reuse
previous results.

#### Scenario: Normal maps still respond
- **WHEN** directional lightmaps are baked
- **THEN** a normal-mapped surface SHALL still show normal-map detail in indirect lighting

#### Scenario: Stationary light with shadow mask
- **WHEN** a light is marked `Stationary` with shadow-mask baking
- **THEN** its indirect contribution SHALL be baked while its direct light is computed at runtime
  using the baked shadow term

#### Scenario: Incremental rebake
- **WHEN** one object is moved in a large baked level
- **THEN** the bake SHALL re-solve the affected region rather than the whole level

#### Scenario: Bake seeds the dynamic caches
- **WHEN** a baked level runs in `Hybrid` mode
- **THEN** the dynamic caches SHALL be initialised from the bake rather than from zero

### Requirement: UV2 and chart packing
Static meshes participating in lightmapping SHALL have a **UV2** channel generated at import time
if not authored, using automatic unwrapping with a configurable texel density, chart padding, and
angle and area distortion limits.

Charts SHALL be packed into atlases with padding sufficient for bilinear filtering and mip
generation at the bake resolution.

#### Scenario: Unwrap is cached
- **WHEN** a mesh is reimported without geometry changes
- **THEN** the cached UV2 unwrap SHALL be reused

#### Scenario: Seam artifacts
- **WHEN** charts meet at a seam
- **THEN** border texels SHALL be dilated and seam colours reconciled so the seam is not visible

### Requirement: Irradiance volumes and light probes
The engine SHALL support **irradiance volumes**: 3D grids of baked probes storing spherical
harmonics irradiance, used to light dynamic objects inside baked environments.

Probes SHALL be placeable as a regular grid within a volume, adaptively subdivided near
geometry, or hand-placed.

Sampling SHALL be trilinear across the eight surrounding probes, weighted by a **visibility**
term so a probe on the other side of a wall does not leak light.

#### Scenario: Dynamic object lit by baked environment
- **WHEN** a character walks through a baked room
- **THEN** it SHALL be lit by interpolated probe irradiance, not by the lightmap

#### Scenario: Light leaking is suppressed
- **WHEN** a probe lies inside geometry or on the far side of a wall
- **THEN** its weight SHALL be reduced or zeroed by the visibility term

### Requirement: Offline path tracer and ground truth
The engine SHALL provide an offline **GPU path tracer** serving three purposes: baking lightmaps
and probes, **seeding** the dynamic caches so a level looks correct before convergence, and
producing **ground-truth references** for validating the real-time result.

The path tracer SHALL share the scene, material, and light representations with the real-time
path, so a difference between them is a real difference rather than a modelling discrepancy.

A **comparison mode** SHALL render the real-time result and the path-traced reference for the same
view and report the error, per region and in aggregate.

#### Scenario: Baked data seeds dynamic GI
- **WHEN** a level with baked data starts in dynamic mode
- **THEN** the caches SHALL be seeded from the bake, and the first frame SHALL be plausible rather
  than converging from black

#### Scenario: Real-time result is measurable
- **WHEN** a GI change is proposed
- **THEN** the error against the path-traced reference SHALL be computable for a set of test views

#### Scenario: Shared representations
- **WHEN** the path tracer and the real-time path light the same scene
- **THEN** they SHALL use the same materials, lights, and geometry, so any divergence is
  attributable

### Requirement: Illumination diagnostics
The system SHALL answer causal questions, not only display buffers. It SHALL be able to report,
for a selected pixel or region: which tier answered it, the confidence of each contributing
source, which probes contributed and their age, whether the region is invalidated or unconverged,
and what geometry contributed most to the cost.

A profiler view SHALL report per frame: the GI allocation and measured cost broken down by stage
(screen trace, world trace, probe update, surface cache, denoise, reflection resolve), probe
counts and updates, rays traced per tier, screen-trace hit rate, and cache hit rate.

#### Scenario: Why is this area dark
- **WHEN** a region appears wrongly dark
- **THEN** the developer SHALL be able to query it and learn whether the cause is occlusion,
  an unconverged region, missing probes, or a low-confidence source

#### Scenario: Cost is attributable to content
- **WHEN** GI cost is high
- **THEN** the profiler SHALL attribute it to stages and to the geometry or lights responsible

### Requirement: GI debug visualisation
The engine SHALL provide debug views: lightmap UV density and charts, lightmap texel resolution,
probe positions, irradiance, age and error, probe visibility, radiance clipmap bounds and
residency, surface cache contents and residency, distance field slices, GI cards, reflection probe
influence volumes and their captures, screen-trace hit and confidence, tier selection per pixel,
software and hardware ray counts, denoiser confidence, disocclusion, and the indirect diffuse,
indirect specular, and emissive contributions in isolation.

#### Scenario: Diagnosing a light leak
- **WHEN** light leaks through a wall
- **THEN** the probe visibility debug view SHALL show which probes contribute incorrectly

#### Scenario: Seeing which tier answered
- **WHEN** the tier selection view is enabled
- **THEN** each pixel SHALL be coloured by the tier that answered its illumination query
