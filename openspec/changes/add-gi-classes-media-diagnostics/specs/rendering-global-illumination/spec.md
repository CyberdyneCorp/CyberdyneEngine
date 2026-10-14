## MODIFIED Requirements

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

A `SlowDynamic` object's moves SHALL be coalesced, and the union of the region it moved through
SHALL be invalidated at most once per configured interval. A `FastDynamic` object's approximate
occlusion SHALL be measured against the offline path tracer with the same body in the scene, with a
stated error.

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

#### Scenario: A character darkens the floor it stands on without invalidating anything
- **WHEN** a character classified `FastDynamic` stands on a lit floor
- **THEN** the indirect light under it SHALL fall as the path tracer says it does, within the
  stated error, and no cache or field SHALL be invalidated

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

The staging table SHALL be queryable, and installing a refraction solver or a caustics source SHALL
be refused with an error naming the deferral. A surface that declares no transmission SHALL resolve
exactly as before transmission existed, and a fog given no indirect grid SHALL march exactly as
before.

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

### Requirement: Illumination diagnostics
The system SHALL answer causal questions, not only display buffers. It SHALL be able to report,
for a selected pixel or region: which tier answered it, the confidence of each contributing
source, which probes contributed and their age, whether the region is invalidated or unconverged,
and what geometry contributed most to the cost.

A profiler view SHALL report per frame: the GI allocation and measured cost broken down by stage
(screen trace, world trace, probe update, surface cache, denoise, reflection resolve), probe
counts and updates, rays traced per tier, screen-trace hit rate, and cache hit rate.

A point query SHALL resolve the point with the same function the resolve uses, so its answer is the
resolve's answer bit for bit, and SHALL name each contributing probe the distance field says cannot
see the point as a leak suspect.

#### Scenario: Why is this area dark
- **WHEN** a region appears wrongly dark
- **THEN** the developer SHALL be able to query it and learn whether the cause is occlusion,
  an unconverged region, missing probes, or a low-confidence source

#### Scenario: Cost is attributable to content
- **WHEN** GI cost is high
- **THEN** the profiler SHALL attribute it to stages and to the geometry or lights responsible

#### Scenario: A leak is named
- **WHEN** a probe on the far side of a wall contributes to a point
- **THEN** the query SHALL report it as a leak suspect with its share of the weight

### Requirement: GI debug visualisation
The engine SHALL provide debug views: lightmap UV density and charts, lightmap texel resolution,
probe positions, irradiance, age and error, probe visibility, radiance clipmap bounds and
residency, surface cache contents and residency, distance field slices, GI cards, reflection probe
influence volumes and their captures, screen-trace hit and confidence, tier selection per pixel,
software and hardware ray counts, denoiser confidence, disocclusion, and the indirect diffuse,
indirect specular, and emissive contributions in isolation.

Each view SHALL be a frame view mode selectable by name, drawn from the same functions that compute
the illumination it shows, and held to a committed golden image.

#### Scenario: Diagnosing a light leak
- **WHEN** light leaks through a wall
- **THEN** the probe visibility debug view SHALL show which probes contribute incorrectly

#### Scenario: Seeing which tier answered
- **WHEN** the tier selection view is enabled
- **THEN** each pixel SHALL be coloured by the tier that answered its illumination query
