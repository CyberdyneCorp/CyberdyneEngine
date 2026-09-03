## ADDED Requirements

### Requirement: Weather publishes state
Weather SHALL influence the world by **writing environment fields** (see `environment-fields`). It
SHALL NOT iterate materials, foliage instances, water bodies, or particle systems to apply itself.

Consumers SHALL sample the fields they need at the fidelity they need: materials sample wetness,
foliage samples wind, water samples precipitation, audio samples wind and rain, artificial
intelligence samples visibility.

There SHALL NOT be a central weather component that pushes state to subsystems.

#### Scenario: Rain does not touch materials
- **WHEN** it rains
- **THEN** the wetness field SHALL change and materials SHALL sample it, with no per-material update

#### Scenario: A new consumer needs no weather change
- **WHEN** a new system needs to react to wind
- **THEN** it SHALL sample the wind field, and weather SHALL be unmodified

### Requirement: Climate, weather, and presentation
The engine SHALL separate three layers:

| Layer | Describes | Changes over |
|---|---|---|
| **Climate** | Prevailing tendencies: mean temperature, humidity, prevailing wind, rainfall potential, solar exposure, ocean influence | Rarely; mostly authored or generated |
| **Weather** | Current environmental state: temperature, humidity, pressure, wind, precipitation, cloud coverage, visibility | Minutes to hours |
| **Presentation** | How that state is drawn and heard | Per frame |

Presentation SHALL NOT be authoritative for weather, and climate SHALL NOT be recomputed to answer a
question about the current state.

Climate SHALL feed **biome potential** (see `environment-fields`), which procedural generation
consumes.

#### Scenario: Graphics are not the weather
- **WHEN** cloud rendering quality is reduced
- **THEN** the weather state SHALL be unchanged

#### Scenario: Climate feeds generation
- **WHEN** biomes are generated
- **THEN** they SHALL derive from climate fields rather than from current weather

### Requirement: Weather cells and hierarchy
Weather state SHALL be maintained over **weather cells** at a resolution appropriate to the world,
typically kilometres, refined near streaming sources where higher fidelity is needed.

Weather SHALL be **hierarchical**: global and continental tendencies inform regional conditions,
which inform local conditions, which are modified by terrain.

Weather cells SHALL NOT be required to align with world partition cells, PCG regions, or field tiles;
each system's partition serves its own purpose.

Full computational meteorology SHALL NOT be attempted. The model SHALL be plausible and controllable:
advection of state with wind, storm phenomena, and terrain influence such as rain shadow and
orographic effects.

#### Scenario: Weather spans the world cheaply
- **WHEN** a hundred-kilometre world is simulated
- **THEN** weather SHALL be maintained over coarse cells at low cost, refined only near viewers

#### Scenario: Terrain shapes weather
- **WHEN** a mountain range lies across the prevailing wind
- **THEN** rainfall SHALL be higher windward and lower leeward, from a declared model rather than
  hand-painted

### Requirement: Environment sampling
The engine SHALL provide an **environment sample** at a world position: temperature, humidity,
pressure, wind, precipitation rate by type, cloud coverage, visibility, and derived values such as
wetness potential.

Sampling SHALL declare a **quality**: macro for strategic reasoning, gameplay for simulation, and
high-frequency for effects near the camera — so that artificial intelligence does not pay for detail
it cannot use.

Sampling SHALL be batchable and SHALL NOT allocate.

Sampling in a region whose fine data is not resident SHALL return the coarsest resident value with a
resolution indicator, and SHALL NOT block.

#### Scenario: Strategic reasoning is cheap
- **WHEN** an agent evaluates conditions across a map
- **THEN** it SHALL sample at macro quality without forcing fine data resident

#### Scenario: Effects get detail
- **WHEN** an effect near the camera samples wind
- **THEN** it SHALL be able to request high-frequency detail

### Requirement: The wind field
Weather SHALL be the producer of the **wind field** defined in `environment-fields`, composing:
prevailing wind from climate, regional wind from weather cells and storms, terrain influence
(blocking, channelling, ridge acceleration), authored local volumes, and transient sources.

**Local wind volumes** SHALL be supported — directional, vortex, radial blast, updraft, downdraft, and
spline-following — for tornadoes, ventilation, thrusters, explosions, and canyon wind, blended into
the field rather than applied to consumers directly.

Wind SHALL carry base velocity, gust, turbulence, and vertical components, so consumers can use the
component they need.

Every wind consumer — foliage, VFX, cloth, water, clouds, audio, and gameplay where it applies —
SHALL sample this field. No subsystem SHALL implement its own wind.

#### Scenario: Everything agrees about the wind
- **WHEN** trees, smoke, water, and cloth are visible together
- **THEN** all SHALL sample one field and move consistently

#### Scenario: A tornado is a volume
- **WHEN** a tornado crosses the map
- **THEN** it SHALL contribute to the wind field, and consumers SHALL respond without tornado-specific
  integration

### Requirement: Precipitation
Weather SHALL produce **precipitation** by type — rain, snow, hail, ash, dust, and project-defined —
with an intensity published as field state.

Presentation SHALL be tiered: effects near the camera, screen-space or volumetric approximation at
middle distance, and field state alone at distance.

**Individual precipitation particles SHALL NOT be physically simulated or collided.** Whether a
position is sheltered SHALL be answered by a **precipitation occlusion representation** — a coarse
sky-visibility structure derived from scene geometry — not by a ray per drop.

Interaction effects — splashes, drips, surface impacts — SHALL be driven by that occlusion and by the
wetness field rather than by particle collision events.

#### Scenario: Indoors is dry
- **WHEN** a character stands under a roof
- **THEN** precipitation effects SHALL be suppressed there through the occlusion representation

#### Scenario: Rain scales
- **WHEN** heavy rain falls across a large view
- **THEN** it SHALL be rendered in tiers rather than as millions of simulated drops

### Requirement: Wetness and snow accumulation
Precipitation SHALL accumulate into **wetness** and **snow depth** fields, and both SHALL **decay**:
wetness by evaporation driven by temperature, sun exposure, and wind; snow by melting driven by
temperature and sun.

Materials SHALL sample wetness and snow rather than being told about them, and visual accumulation
SHALL be composited through the runtime virtual texture path (see `virtual-texturing`) rather than by
modifying geometry.

Accumulation SHALL be persistable where a project requires it: a region that was snowed on and
unloaded SHALL retain its snow state.

Foliage, terrain, water, navigation, and gameplay SHALL be able to consume these fields.

#### Scenario: Surfaces dry out
- **WHEN** rain stops and the sun comes out
- **THEN** wetness SHALL decay at a rate driven by temperature, sun, and wind

#### Scenario: Snow is not geometry
- **WHEN** snow accumulates
- **THEN** it SHALL be a field composited into surfaces, not a modification of terrain geometry

### Requirement: Storm phenomena
Storms SHALL be modelled as **spatial phenomena** with identity, position, velocity, radius,
intensity, and type — moving across the world and contributing to pressure, wind, precipitation, cloud
coverage, and lightning potential.

Many storms SHALL be able to exist independently, and a storm SHALL be a first-class object that can
be queried, replicated, recorded, and scripted.

**Lightning** SHALL be published as an event with position, intensity, and a seed, consumed by
effects, audio, illumination, and gameplay. Weather SHALL NOT play a sound or spawn an effect
directly.

Thunder timing SHALL be derived by the audio system from distance, not scheduled by weather.

#### Scenario: A storm crosses the map
- **WHEN** a storm moves
- **THEN** the fields under it SHALL change as it passes, and consumers SHALL follow

#### Scenario: Lightning is an event
- **WHEN** lightning strikes
- **THEN** weather SHALL publish an event and effects, audio, and lighting SHALL respond to it

### Requirement: Weather presets and transitions
Weather SHALL be authorable as **presets** describing a target environmental state — clear, overcast,
light rain, storm, snowstorm, sandstorm, and project-defined — not merely a visual configuration.

Applying a preset SHALL **transition** toward it rather than switching, with **per-property
durations**: cloud coverage over minutes, precipitation over a shorter period, wind over another.

Transitions SHALL be deterministic where the session's determinism profile requires it, and
schedulable in advance so that a designed weather sequence is reproducible.

Sequencing tools SHALL drive weather by manipulating this state; a separate cinematic-only weather
implementation SHALL NOT exist.

#### Scenario: Weather arrives, it does not appear
- **WHEN** a storm preset is applied
- **THEN** cloud coverage, wind, and rain SHALL transition on their own time constants

#### Scenario: Cinematics use the same system
- **WHEN** a sequence drives the weather
- **THEN** it SHALL set the same state, not a parallel visual path

### Requirement: Weather determinism and the firewall
Weather SHALL declare which of its state is **authoritative** — participating in gameplay and
therefore deterministic or server-authoritative — and which is **visual detail**.

Where weather affects gameplay — visibility affecting detection, wind affecting projectiles, storms
slowing movement — that state SHALL be authoritative and SHALL follow the session's determinism
profile.

**Visual weather detail SHALL NOT influence authoritative state.** Cloud voxels, precipitation
particles, and GPU-simulated detail are presentation, as required by the determinism firewall in
`simulation-and-determinism`.

Authoritative weather evolution SHALL be reproducible from its seed and recorded events, or the
authority SHALL record its state deltas.

#### Scenario: Rain slows units reproducibly
- **WHEN** precipitation affects movement in a competitive session
- **THEN** that state SHALL be authoritative and identical on every peer

#### Scenario: Particles do not affect gameplay
- **WHEN** a client renders heavier rain at a higher quality tier
- **THEN** gameplay outcomes SHALL be unchanged

### Requirement: Weather in replay and networking
Replays SHALL reconstruct weather from its **seed and deterministic evolution**, or from recorded
authoritative state deltas and events where evolution is not deterministic.

A replay SHALL NEVER need to record cloud volumes, particle state, or rendered output.

Networking SHALL replicate **low-frequency authoritative state**: storm identity, position, velocity,
intensity, regional weather state, and events such as lightning. Clients SHALL reconstruct visual
detail locally.

Weather state SHALL be part of the session's persistent state where a project requires weather to
survive a save.

#### Scenario: A storm costs little bandwidth
- **WHEN** a storm crosses a multiplayer map
- **THEN** its identity, motion, and intensity SHALL be replicated, and clouds SHALL be reconstructed
  on each client

#### Scenario: A replay reproduces the weather
- **WHEN** a session is replayed
- **THEN** weather SHALL be reconstructed from seed and recorded state, not from recorded visuals

### Requirement: Ecosystem state
Weather and environment SHALL maintain **macro ecosystem state** as fields — vegetation density,
biomass, forest age, soil health, burn fraction, moisture — evolving at low resolution over long
periods, whether or not a region is resident.

Ecosystem state SHALL evolve toward **biome potential** and be knocked back by events: fire,
deforestation, drought, pollution, or terraforming.

Per-organism ecological simulation SHALL NOT be attempted. Detail is materialised by procedural
generation from macro state when a region becomes relevant (see `procedural-content-generation`).

Thresholds between biome states SHALL be expressible as declared conditions over fields, so that a
world can transition from desert to savanna to forest as conditions change.

#### Scenario: A burned forest regrows
- **WHEN** a region burns and time passes
- **THEN** its burn fraction SHALL decay and vegetation density SHALL recover toward potential,
  without simulating individual plants

#### Scenario: Terraforming changes a biome
- **WHEN** soil health and moisture cross declared thresholds
- **THEN** the region's biome state SHALL change, and generated content SHALL follow on
  materialisation

### Requirement: Environment budgets
Weather simulation, field updates, cloud rendering, volumetrics, and precipitation presentation SHALL
each hold declared budgets: processor time, GPU time, field memory, and field update bandwidth.

Presentation budgets SHALL be levers of the renderer budget arbiter, and **reducing them SHALL NOT
alter authoritative weather state**.

Simulation budgets SHALL bound weather evolution cost independently of view, so that a large world's
weather cost does not scale with what is on screen.

#### Scenario: Cost is bounded, state is not degraded
- **WHEN** GPU budget pressure reduces cloud and volumetric quality
- **THEN** wind, rain intensity, and visibility values SHALL be unchanged

#### Scenario: Weather cost does not follow the camera
- **WHEN** the camera moves across a large world
- **THEN** weather simulation cost SHALL remain bounded by its own budget

### Requirement: Environment diagnostics
The engine SHALL provide an environment inspector reporting, for a selected position: temperature,
humidity, pressure, wind with its contributing sources, precipitation rate and type, cloud coverage,
visibility, wetness, snow depth, climate region, and the weather sources that produced them.

It SHALL **explain composition**: a rain rate reported as a storm's contribution, a terrain rain
shadow reduction, and a local volume's addition, rather than as one number.

Wind SHALL be visualisable as vectors with per-source contribution, and weather cells, storm paths,
and field values SHALL be visualisable over the world.

#### Scenario: Why is it raining here
- **WHEN** a developer queries a position
- **THEN** the inspector SHALL show which storm, terrain effect, and local volume contributed to the
  rate

#### Scenario: Wind sources are separable
- **WHEN** wind is inspected
- **THEN** prevailing, storm, terrain, and local contributions SHALL be shown separately alongside
  the result

### Requirement: Time simulation tooling
The editor SHALL support **advancing environmental and ecosystem state rapidly** without rendering
intermediate frames — simulating days, seasons, or years of weather, wetness, snow, vegetation
recovery, and biome change.

After advancing, procedural generation SHALL be able to rematerialise local detail consistent with
the new state, so that a designer can see what a region looks like after a drought, a fire, or a
terraforming programme.

This SHALL use the same macro state and generation path as the runtime, not a separate preview model.

#### Scenario: Ninety days in a moment
- **WHEN** a designer advances the environment by ninety days
- **THEN** snow melt, vegetation recovery, and moisture change SHALL be applied and the world
  rematerialised accordingly

#### Scenario: Preview is not a separate model
- **WHEN** the editor simulates forward
- **THEN** it SHALL use the runtime's macro model, so the preview matches what will happen

### Requirement: Forbidden environment patterns
The following SHALL NOT appear, and each SHALL be checkable:

- Weather directly mutating materials, foliage instances, water bodies, or particle systems
- Wind implemented independently in foliage, VFX, clouds, cloth, water, or audio
- Gameplay behaviour derived from cloud voxels, precipitation particles, or GPU-simulated detail
- Per-drop physics simulation or per-drop collision for precipitation
- Cloud shadows produced through the virtual shadow page system
- A world-scale volumetric cloud field stored, streamed, or replicated
- Visual quality settings altering authoritative weather state
- Weather cells assumed to align with world cells, PCG regions, or field tiles
- A cinematic-only weather implementation parallel to the runtime one

#### Scenario: A proposal is checked
- **WHEN** a change would iterate materials to apply wetness
- **THEN** it SHALL be flagged against this requirement
