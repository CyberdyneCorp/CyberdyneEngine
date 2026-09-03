## ADDED Requirements

### Requirement: Physically based atmosphere
The engine SHALL provide a **participating atmosphere** model parameterised by physical quantities:
planet and atmosphere radii, Rayleigh scattering coefficients and scale height, Mie scattering,
absorption and anisotropy, ozone or equivalent absorption, ground albedo, and stellar illuminance.

The model SHALL support atmospheres that are not Earth's — thin, dusty, dense, or chemically
different — so that a planet's sky follows from its parameters rather than from a tinted preset.

Atmosphere parameters SHALL be authorable as an **environment profile** asset, and SHALL be
changeable at runtime, including gradually, so that terraforming or a global event can alter the sky.

#### Scenario: A different planet
- **WHEN** a project sets a dusty thin atmosphere
- **THEN** sky colour, aerial perspective, and sunlight tint SHALL follow from the scattering
  parameters

#### Scenario: The sky changes over time
- **WHEN** atmospheric composition changes during play
- **THEN** the sky SHALL follow, and the change SHALL be a parameter transition rather than a preset
  switch

### Requirement: Precomputed atmospheric tables
Sky evaluation SHALL use **precomputed lookup tables** — transmittance, multiple scattering, sky
view, and aerial perspective — rather than ray marching the atmosphere per pixel.

Tables SHALL be regenerated only when the parameters they depend on change, and regeneration SHALL be
incremental where a parameter changes continuously, such as a moving sun.

Table resolution SHALL be a quality setting, and their generation cost SHALL be reported.

#### Scenario: A moving sun is cheap
- **WHEN** the sun moves continuously
- **THEN** affected tables SHALL update incrementally rather than being fully regenerated each frame

#### Scenario: Sky is a lookup
- **WHEN** the sky is rendered
- **THEN** it SHALL sample tables rather than integrating the atmosphere per pixel

### Requirement: Aerial perspective
Distance attenuation SHALL be produced by the **atmosphere model** — scattering and transmittance
over distance — and applied to opaque shading and to volumetric media consistently.

Ad-hoc distance fog with independently tuned parameters SHALL NOT be the engine's model of distance,
though a stylised override SHALL remain available for projects that want one.

Aerial perspective SHALL be correct at large scale, so that terrain kilometres away is attenuated
consistently with the sky above it.

#### Scenario: Distance is not separately tuned
- **WHEN** distant terrain is rendered
- **THEN** its attenuation SHALL come from the atmosphere, consistent with the sky

#### Scenario: Stylisation is explicit
- **WHEN** a project wants non-physical distance falloff
- **THEN** it SHALL be a declared override rather than a divergence between two models

### Requirement: Celestial bodies and time of day
The engine SHALL model **celestial bodies** — a star, moons, and other bodies — as environmental
light and sky sources, with direction and intensity derived either from a celestial model (time,
latitude, planetary parameters) or from direct authored control.

Realistic astronomy SHALL NOT be required; a project SHALL be able to place its sun by hand.

**Time of day** SHALL advance on a declared time domain, mapping from gameplay, real, cinematic, or
custom time, so that a day may be minutes or hours and may be paused or scripted independently of
gameplay time.

Time of day SHALL drive sun direction and illuminance, sky state, star visibility, and the tendencies
that feed weather, and gameplay SHALL be able to override any of them.

#### Scenario: A short day
- **WHEN** a project maps one real minute to twenty game minutes
- **THEN** the celestial model SHALL follow that mapping, and sky, lighting, and shadows SHALL
  follow it

#### Scenario: Authored sun
- **WHEN** a project places the sun directly for art direction
- **THEN** the celestial model SHALL be bypassable without losing atmosphere or sky

### Requirement: Sky composition
The sky SHALL be composed from: the atmosphere, celestial bodies, stars and background sky content,
clouds, and optional phenomena such as aurorae.

The sky SHALL be rendered as a dedicated path producing radiance for the background, a filtered
radiance map for specular image-based lighting, and irradiance for ambient diffuse — consumed by
`rendering-global-illumination`.

Background sky content SHALL be supportable as procedural stars, a star catalogue, or authored
imagery, and the choice SHALL be a quality and content decision rather than a structural one.

#### Scenario: Illumination follows the sky
- **WHEN** cloud cover thickens
- **THEN** the radiance and irradiance the illumination system consumes SHALL change accordingly

#### Scenario: Night is not a texture by necessity
- **WHEN** a project wants a procedural night sky
- **THEN** stars SHALL be generatable rather than requiring authored imagery

### Requirement: Cloud representation
Clouds SHALL be represented as a **low-resolution weather map** — coverage, type, and moisture over
kilometres — combined with procedural detail: noise, erosion, and a height profile per layer.

A world-scale volumetric cloud field SHALL NOT be stored, streamed, or replicated. Density SHALL be
**reconstructed procedurally** during evaluation from the map and the noise.

Multiple **cloud layers** SHALL be supported — low, middle, high, storm, and project-defined — each
with base altitude, thickness, coverage, density, type, and wind.

Layer parameters SHALL be driven by weather state rather than authored per frame.

#### Scenario: Clouds are compact
- **WHEN** clouds cover a hundred-kilometre world
- **THEN** what is stored and replicated SHALL be a coarse weather map, not a volume

#### Scenario: Layers follow weather
- **WHEN** a storm approaches
- **THEN** the storm layer's coverage and density SHALL follow the weather state

### Requirement: Cloud rendering
Volumetric clouds SHALL be rendered by ray marching the reconstructed density, with lighting
accounting for direct light, an approximation of multiple scattering, ambient sky, self-shadowing,
and atmospheric transmittance.

Rendering SHALL run at **reduced resolution with temporal reconstruction** through the framework in
`temporal-rendering`, with cloud-specific history semantics: clouds move independently of the camera,
so reprojection SHALL account for cloud motion and history SHALL be rejected where weather changed.

Quality SHALL be a tier: ray march step count, resolution, and scattering approximation SHALL be
budget levers held by the renderer budget arbiter.

Lower cloud quality SHALL NOT change weather state — only how it is drawn.

#### Scenario: Quality does not change the weather
- **WHEN** cloud quality is reduced under budget pressure
- **THEN** rain intensity, wind, and visibility SHALL be unchanged

#### Scenario: Clouds reproject correctly
- **WHEN** clouds drift while the camera is still
- **THEN** temporal reconstruction SHALL account for their motion rather than smearing

### Requirement: Cloud shadows
Clouds SHALL cast shadows onto the world through a **coarse world-scale shadow representation** — a
low-frequency field or map covering a large area at low resolution — consumed by terrain, foliage,
water, and illumination.

Cloud shadows SHALL NOT be produced through the virtual shadow page system, whose design assumes
shadow detail correlates with screen pixels; a cloud shadow's footprint is kilometres wide and its
detail is low-frequency.

Cloud shadow resolution and update rate SHALL be budget levers.

#### Scenario: A cloud shadow crosses a valley
- **WHEN** clouds drift over terrain
- **THEN** a coarse shadow field SHALL darken the affected area, sampled by surfaces and by
  illumination

#### Scenario: The right mechanism
- **WHEN** cloud shadows are implemented
- **THEN** they SHALL use the coarse field rather than allocating virtual shadow pages

### Requirement: Volumetric integration
Clouds, fog, mist, smoke, and atmospheric volumes SHALL share the engine's **volumetric
infrastructure** rather than each building its own pipeline: the froxel volume defined in
`rendering-post-processing` for local media, with clouds using a specialised long-range
representation because their scale differs by orders of magnitude.

VFX SHALL be able to inject density and emission into the local volumetric medium rather than
producing an unrelated volumetric effect.

Weather SHALL influence local volumetrics through **visibility and aerosol density** published as
state, not by writing fog parameters directly.

#### Scenario: One medium near the camera
- **WHEN** fog, mist, and smoke are present together
- **THEN** they SHALL occupy one volumetric medium and be lit consistently

#### Scenario: Weather influences fog through state
- **WHEN** humidity rises
- **THEN** visibility SHALL change and the fog parameters SHALL follow from it

### Requirement: Planetary scale
Atmosphere, sky, and clouds SHALL be correct from ground level through mountains and aircraft
altitude to orbit, where the project requires it.

Evaluation SHALL use the world's large-coordinate representation for camera position and SHALL remain
camera-relative for rendering, so precision does not degrade at altitude or distance.

The horizon, atmospheric thickness with altitude, and the transition to space SHALL follow from the
model rather than from separate implementations per altitude band.

#### Scenario: Climbing out of the atmosphere
- **WHEN** a camera ascends from the ground to orbit
- **THEN** the sky SHALL transition continuously without switching to a different implementation

### Requirement: Environment profiles
Atmosphere, sky, celestial, and cloud configuration SHALL be authorable as an **environment profile**
asset, and a project SHALL be able to define several — an Earth-like world, a dusty desert planet, a
toxic volcanic world, or successive stages of a terraforming process.

Profiles SHALL be **transitionable**: a project SHALL be able to move between them over time with
per-parameter transition behaviour, rather than switching instantly.

Profiles SHALL compose with weather presets rather than duplicating them: the profile describes the
world's atmosphere, the preset describes today's weather.

#### Scenario: A world is terraformed
- **WHEN** a planet moves through terraforming stages
- **THEN** its environment profile SHALL transition, and sky, light, and clouds SHALL follow

### Requirement: Atmosphere quality tiers
Atmosphere, sky, and cloud rendering SHALL provide quality tiers spanning mobile to cinematic:
table-based atmosphere with simple layered clouds at the low end, volumetric clouds with shadows and
temporal reconstruction in the middle, and higher ray march counts with advanced scattering at the
top.

**The same environmental state SHALL drive every tier.** A tier SHALL change how the sky is drawn,
never what the weather is.

Tier selection SHALL follow the renderer profile and the budget arbiter.

#### Scenario: One state, four tiers
- **WHEN** the same scene renders on mobile and on a cinematic tier
- **THEN** both SHALL depict the same weather, differing in fidelity and cost

### Requirement: Atmosphere and cloud diagnostics
The engine SHALL provide: visualisation of the cloud coverage field and cloud type, ray march step
counts, temporal history confidence, the cloud shadow field, density slices, the atmospheric tables
and their update frequency, and the celestial model's state.

For any pixel of sky or cloud, the tooling SHALL be able to report what determined it: coverage, type,
layer, the weather source, and the tables sampled.

Cloud rendering cost SHALL be attributable to steps, resolution, and layers, since cloud rendering is
notoriously difficult to tune without that.

#### Scenario: Tuning is directed
- **WHEN** cloud rendering is expensive
- **THEN** the diagnostics SHALL attribute cost to ray march steps, resolution, and layer count

#### Scenario: A cloud's cause is visible
- **WHEN** a developer inspects a cloud formation
- **THEN** the tooling SHALL name the weather map values and the layer that produced it
