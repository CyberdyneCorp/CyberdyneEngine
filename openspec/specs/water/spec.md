# water Specification

## Purpose

Defines **CyberWater**: oceans, lakes, rivers, and pools as one **water body** abstraction over
several simulation backends, rather than four systems that happen to be wet.

The requirement that matters most is the **displacement contract**. The classic water bug is a boat
floating on a flat plane beneath visible swell, and it happens because rendering displaces the
surface on the GPU while physics samples something else. So a body declares which frequency bands
are *authoritative* — evaluated identically for rendering, physics, and gameplay — and which are
*visual only*. Large swell is authoritative; capillary detail is declared, not assumed.

The shoreline is where terrain and water meet, and ownership has to go somewhere or it becomes a
shared mutable mess. It goes to water, which reads terrain and **writes wetness, water distance,
and flow into environment fields**. Terrain never calls into water; it reads a field. That keeps
the two systems most likely to become circularly dependent acyclic.

Weather and hydrology are specified as seams: wind, precipitation, and temperature are consumed as
fields, so a future weather capability becomes a producer without changing anything here.

## Requirements

### Requirement: Water bodies
Water SHALL be modelled as **water bodies**: an identity, a type, a material, a simulation backend,
a mean level, bounds, and streaming metadata.

Types SHALL include at minimum: ocean, lake, river, pool, and waterfall. They SHALL be one
abstraction with different backends, not separate systems.

A world MAY contain many bodies simultaneously, and a position MAY be inside more than one, with a
declared resolution order.

#### Scenario: One abstraction, several backends
- **WHEN** an ocean, a lake, and a river are present
- **THEN** they SHALL be water bodies differing in simulation backend, and every consumer SHALL
  query them through one interface

#### Scenario: Overlapping bodies
- **WHEN** a river meets the sea
- **THEN** the overlap SHALL be resolved by a declared order, and queries SHALL return one
  consistent answer

### Requirement: Simulation backends
A water body SHALL declare a **simulation backend**, and backends SHALL be replaceable behind one
interface:

| Backend | Used for | Status |
|---|---|---|
| `Flat` | Distant or simple water | Required |
| `Spectral` | Oceans and lakes | Required |
| `SplineFlow` | Rivers | Required |
| `ShallowWater` | Shorelines, floods, gameplay water | Planned |
| `Particle` | Small-volume interactive fluid | Deferred |

The particle and volumetric fluid tier SHALL be deferred, and its seam SHALL be the same seam
`vfx-system` reserves for fluids; the two SHALL NOT become separate fluid systems.

#### Scenario: Backend is a body property
- **WHEN** a lake is configured to use a smaller spectral model than the ocean
- **THEN** it SHALL declare that backend, and no consumer code SHALL change

#### Scenario: Fluids stay one seam
- **WHEN** an interactive fluid tier is introduced
- **THEN** it SHALL satisfy both this backend interface and the VFX fluid seam, rather than
  creating a second fluid system

### Requirement: The displacement contract
Every water body SHALL declare which of its displacement frequency bands are **authoritative** and
which are **visual only**.

Authoritative bands SHALL be evaluated **identically** for rendering, physics queries, and gameplay
queries, from one shared definition. Large swell SHALL be authoritative.

Visual-only bands MAY be evaluated in rendering alone, and SHALL be declared, so the discrepancy is
documented rather than discovered.

A visual-only band whose amplitude is large enough to be felt by gameplay SHALL be a specification
violation, and the engine SHALL be able to report the amplitude split so it is checkable.

#### Scenario: A boat floats on the waves it appears to float on
- **WHEN** a vessel floats on a rough sea
- **THEN** its buoyancy SHALL be computed from the same authoritative displacement the renderer
  uses, and it SHALL NOT sit on a flat plane beneath visible swell

#### Scenario: Fine detail is declared visual
- **WHEN** capillary detail is added for close-up appearance
- **THEN** it SHALL be declared visual-only, and its amplitude SHALL be reportable

### Requirement: Ocean simulation
Oceans SHALL be simulated by **cascaded spectral synthesis**: several frequency bands, each covering
a wavelength range, driven by wind speed, direction, and fetch, producing displacement, normals,
and a foam or breaking indicator.

A single world-scale simulation SHALL NOT be used; cascades SHALL be evaluated at appropriate
resolutions and combined.

The ocean surface SHALL be **camera-relative** for rendering — generated around the viewer rather
than as a world-scale mesh — while its parameters (sea level, wind, tide, weather) remain global.

Ocean surface geometry SHALL be produced procedurally and published into the GPU scene, so it is
culled and shaded like other geometry.

#### Scenario: No ocean mesh exists
- **WHEN** an ocean spans the world
- **THEN** its surface SHALL be generated around the camera, and no world-scale ocean mesh SHALL
  be stored or streamed

#### Scenario: Wind changes the sea
- **WHEN** the wind field's strength increases
- **THEN** the spectrum SHALL change and the sea state SHALL follow, consistently for rendering and
  for authoritative queries

### Requirement: Rivers
Rivers SHALL be authored as **spline networks** supporting junctions and branches, with each
section declaring width, depth, bed profile, water level, flow speed, turbulence, and material.

The system SHALL generate from the network: the surface, a **flow field**, shoreline data, foam
sources, and buoyancy and collision data.

Flow SHALL be modified by geometry — accelerating in narrows, deflecting around obstacles, forming
turbulence at bends and drops — rather than being uniform along the spline.

Rivers SHALL write flow and water distance into environment fields, so terrain materials, foliage,
audio, and gameplay read them without querying the water system.

#### Scenario: A tributary joins
- **WHEN** two river branches meet
- **THEN** the junction SHALL produce a continuous surface and a combined flow field, not two
  overlapping surfaces

#### Scenario: Flow drives everything downstream
- **WHEN** debris enters a river
- **THEN** it SHALL be carried by the flow field, and particles, audio, and AI SHALL read the same
  field

### Requirement: Shoreline
The **shoreline** SHALL be computed jointly from terrain and water: water level and wave approach
against terrain height, slope, and material.

Shoreline computation SHALL be **owned by water**, which reads terrain and writes **wetness**,
**water distance**, and **water depth** into environment fields. Terrain SHALL NOT query the water
system directly; it SHALL read those fields.

This dependency direction SHALL be maintained so that terrain and water do not become mutually
dependent.

Shoreline data SHALL drive: foam generation, wetness darkening and roughness change in terrain
materials, wave breaking, and shore VFX and audio.

#### Scenario: A wet shore
- **WHEN** waves wash up a beach
- **THEN** the wetness field SHALL be updated and the terrain material SHALL darken and smooth
  accordingly, without terrain knowing about waves

#### Scenario: The dependency stays acyclic
- **WHEN** terrain material needs to know how close water is
- **THEN** it SHALL sample the water distance field, and no call from terrain into water SHALL
  exist

### Requirement: Water surface shading
Water SHALL be shaded through a **water surface closure** in the material system (see
`material-compiler` and `rendering-materials-and-shading`), not forced through the opaque
metallic-roughness model.

The closure SHALL account for: reflection, refraction, wavelength-dependent **absorption** and
**scattering** through the water column, surface roughness, normals, and foam coverage.

Colour with depth SHALL follow physically-inspired attenuation over the water column thickness,
so deep water darkens and shifts hue without hand-authored gradients.

Reflections SHALL use the illumination hierarchy (see `rendering-global-illumination`): screen
tracing first, escalating to world or hardware tracing by confidence and roughness, with distant
rough water resolved from cached radiance.

#### Scenario: Depth changes colour physically
- **WHEN** water deepens away from a shore
- **THEN** its colour SHALL change through absorption over the water column rather than through a
  painted gradient

#### Scenario: Rough water does not trace
- **WHEN** water is rough and distant
- **THEN** its reflection SHALL come from cached radiance rather than dedicated rays

### Requirement: Underwater rendering
When a camera or listener is inside a water body, the engine SHALL apply that **body's parameters**
— absorption, scattering, and colour — to produce underwater appearance, rather than a fixed
fullscreen tint.

Underwater rendering SHALL include: depth-dependent absorption and scattering, participating-media
fog consistent with the volumetric system, caustics, surface distortion viewed from below, and the
audio filtering declared by the audio system.

The transition across the surface SHALL be handled explicitly, including a camera intersecting the
surface, so that the boundary is not a hard switch.

#### Scenario: Two lakes look different underwater
- **WHEN** a camera submerges in a clear lake and then a silty one
- **THEN** the underwater appearance SHALL differ according to each body's absorption and
  scattering parameters

#### Scenario: Crossing the surface
- **WHEN** a camera is half in and half out of the water
- **THEN** the boundary SHALL be rendered explicitly rather than switching between two full-screen
  states

### Requirement: Foam
Foam SHALL be a **persistent advected field** rather than an instantaneous function of the surface:
generated from wave curvature and breaking, shoreline interaction, river turbulence, waterfalls,
and object interaction; advected by the flow or wave velocity; and decaying over a declared
lifetime.

Foam SHALL be bounded in memory and resolution, scoped to regions near streaming sources.

#### Scenario: A wake persists
- **WHEN** a boat crosses open water
- **THEN** its wake foam SHALL persist behind it, drift with the surface, and decay, rather than
  disappearing when the boat moves on

### Requirement: Caustics
Caustics SHALL be supported in tiers: a projected animated approximation, a surface-derived
approximation computed from the water surface, and a traced solution where ray tracing is
available.

The tier SHALL be selected by renderer profile and by the GI budget allocation, and the surface
derived tier SHALL be the default for real-time use.

Caustics SHALL apply both underwater and, where appropriate, to surfaces above shallow water.

#### Scenario: Caustics follow the actual surface
- **WHEN** the surface-derived tier is active
- **THEN** caustic patterns SHALL follow the simulated water surface rather than an unrelated
  looping texture

### Requirement: Water queries
The engine SHALL provide a **water query** returning, for a world position: surface height, surface
normal, velocity, depth to bed, the water column thickness, density, and the identity of the body.

Queries SHALL return the **authoritative** displacement bands, batchable for many positions, and
SHALL NOT block or require a physics query.

A query in a region whose water data is not resident SHALL return the body's mean level with a
resolution indicator.

#### Scenario: Batched hull sampling
- **WHEN** a vessel samples its hull at forty points
- **THEN** the query SHALL be batchable and SHALL return authoritative displacement at each

#### Scenario: Query outside resident data
- **WHEN** a query targets a distant region
- **THEN** it SHALL return the body's mean level and indicate the resolution used

### Requirement: Buoyancy and physics interaction
The engine SHALL provide buoyancy through the physics interface (see `physics`), computed from
submerged volume or from **multiple sample points** on a body, not from a single centre point.

Buoyancy SHALL include: displacement force, linear and angular drag through water, and the effect
of the surface velocity and current, so a boat is carried by a river as well as floated by it.

Buoyancy SHALL use the same authoritative displacement as rendering, and SHALL be deterministic to
the degree the physics determinism requirements demand.

Characters SHALL support swimming and wading states derived from water depth at their position.

#### Scenario: A long vessel pitches with the swell
- **WHEN** a vessel longer than the wavelength floats on swell
- **THEN** sampling at multiple points SHALL make it pitch and roll rather than translate rigidly

#### Scenario: Current carries a floating object
- **WHEN** debris floats in a river
- **THEN** the flow velocity SHALL move it downstream

### Requirement: Water navigation
Water SHALL contribute to navigation (see `navigation`): swimming volumes for submerged navigation,
a surface for vessel navigation, and **directional cost** on flowing water so that upstream travel
costs more than downstream.

Water level changes and flooding SHALL emit navigation dirty regions like terrain change does.

#### Scenario: Downstream is cheaper
- **WHEN** an agent paths along a river
- **THEN** downstream travel SHALL cost less than upstream, through the flow field's directional
  cost

#### Scenario: A flood changes navigation
- **WHEN** an area floods
- **THEN** the affected navigation regions SHALL be invalidated and rebuilt

### Requirement: Water streaming
A water body SHALL exist **logically for the whole world** while only its nearby **segments** are
resident: surface geometry, simulation state, foam, shoreline data, physics representation, and
audio.

A river SHALL NOT be split into unrelated per-cell objects; its network and identity SHALL be
global while its runtime data is segmented.

Water payloads SHALL be a cell channel, so a server profile can omit surface rendering data while
retaining the queries and physics representation it needs.

#### Scenario: A continental river
- **WHEN** a river crosses hundreds of cells
- **THEN** it SHALL be one body with segmented runtime data, and a query anywhere along it SHALL
  identify the same body

#### Scenario: Server keeps queries, drops surfaces
- **WHEN** a dedicated server streams a water region
- **THEN** it SHALL retain query and physics data and omit surface rendering data

### Requirement: Weather and hydrology seams
Water SHALL consume wind, precipitation, and temperature from **environment fields**, so that a
future weather capability becomes a field producer without changing water.

Rainfall driving runoff, streams, river flow, flooding, and erosion SHALL be recorded as a
**hydrology seam**: the inputs and outputs are fields, and a hydrology capability would be a field
producer and consumer rather than a change to terrain or water.

Neither weather nor hydrology is specified by this change. Their absence SHALL NOT block water:
fields may be driven by authored values or by a project.

#### Scenario: Weather arrives later without rework
- **WHEN** a weather capability is introduced
- **THEN** it SHALL produce wind, precipitation, and temperature fields, and water SHALL consume
  them unchanged

#### Scenario: Water works without weather
- **WHEN** no weather system exists
- **THEN** wind and precipitation fields SHALL take authored values and water SHALL function

### Requirement: Water authoring
The editor SHALL provide: ocean configuration, lake and pool authoring by bounds and level, river
authoring by spline network with per-section parameters, waterfall placement, and visualisation of
level, depth, flow, shoreline, and foam sources.

River authoring SHALL show the generated flow field and shoreline as they are edited.

#### Scenario: Editing a river shows its flow
- **WHEN** a designer adjusts a river's width or bed
- **THEN** the generated flow field and shoreline SHALL update in the viewport

### Requirement: Water diagnostics
The engine SHALL report: bodies and their backends, resident segments, simulation cost per body,
displacement band amplitudes with the authoritative and visual split, query counts and cost, foam
field memory, and buoyancy sample counts.

The system SHALL be able to answer, for a position, which body owns it, what the authoritative
surface height is, and which bands contributed.

#### Scenario: The authoritative split is checkable
- **WHEN** a developer suspects physics and rendering disagree
- **THEN** the diagnostics SHALL report the amplitude of authoritative and visual-only bands at
  that location
