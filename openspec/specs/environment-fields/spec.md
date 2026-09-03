# environment-fields Specification

## Purpose

Defines **CyberField**: a sparse, streamed, world-scale data substrate addressed by world position,
sampled by every system that reasons about the environment.

It is specified beneath terrain, foliage, and water rather than inside any of them, and that is the
whole point. Putting moisture inside terrain would make foliage depend on terrain to know whether
the ground is wet; putting wind inside foliage would make water depend on foliage. Each dependency
is defensible alone and the set of them is a knot.

A field declares its semantics, unit, resolution, interpolation, and default, and has **exactly one
producer** — two systems writing one field is a configuration error caught at startup, not a
last-writer-wins race. Fields whose values reach gameplay must declare themselves deterministic,
which rules out producing them on a GPU pass that is never read back deterministically.

The wind field is the clearest illustration of the value: trees, smoke, cloth, water, and audio all
sample one field, so they agree about which way the wind is blowing in a way they never do when
each integrates its own.

## Requirements

### Requirement: Environment fields are a shared substrate
The engine SHALL provide **environment fields**: sparse, tiled, world-scale data addressed by world
position, sampled by any system that reasons about the environment.

Fields SHALL be a capability beneath terrain, foliage, and water — not owned by any of them — so
that no consumer must depend on an unrelated subsystem to read environmental data.

Consumers SHALL include at minimum: terrain materials, foliage placement and state, water,
VFX, audio, AI, navigation cost modifiers, and gameplay.

#### Scenario: Foliage does not depend on terrain for moisture
- **WHEN** foliage placement needs moisture
- **THEN** it SHALL sample the moisture field, and SHALL NOT query the terrain system

#### Scenario: One value, many readers
- **WHEN** terrain material, foliage placement, and audio all need wetness at a point
- **THEN** they SHALL sample the same field and observe the same value

### Requirement: Field declaration
A field SHALL declare: a stable identifier, its value type, its unit and semantic meaning, its
spatial resolution and permitted resolution range, its interpolation mode, its default value where
no data exists, and whether it is static, slowly varying, or per-frame.

Field semantics SHALL be documented and stable, because a field's meaning is a contract between
systems that never call each other.

The engine SHALL define standard fields at minimum: biome, moisture, temperature, soil, snow
depth, water distance, water depth, water flow, wetness, wind, burn state, and human exclusion.

Projects SHALL be able to declare additional fields without engine modification.

#### Scenario: Semantics are explicit
- **WHEN** a system samples `moisture`
- **THEN** its meaning, unit, and range SHALL be defined by the field declaration, not inferred
  from the producer's implementation

#### Scenario: Project field
- **WHEN** a project declares a `radiation` field
- **THEN** it SHALL stream, sample, and debug like a standard field with no engine change

### Requirement: One producer per field
Each field SHALL have exactly **one producer** at any time: a baked source, a system that writes
it, or a project-supplied generator.

Two systems writing one field SHALL be a **configuration error detected at startup or cook time**,
not a last-writer-wins race resolved at runtime.

A field MAY have contributions **layered by declaration** — a base baked layer plus a runtime delta
layer — where the layering and its combination rule are part of the field's declaration rather than
an emergent property of write order.

#### Scenario: Conflicting producers are rejected
- **WHEN** two systems are configured to produce the same field
- **THEN** the conflict SHALL be reported at configuration time and SHALL NOT be resolved by write
  order

#### Scenario: Declared layering
- **WHEN** a field has a baked base and a runtime delta
- **THEN** the combination rule SHALL be part of its declaration and SHALL be applied consistently
  by every reader

### Requirement: Sparse tiled storage and streaming
Fields SHALL be stored as sparse tiles. Regions with no data SHALL consume no storage and SHALL
sample the field's declared default.

Field tiles SHALL be cooked as a **cell channel** and stream with world cells (see
`world-partition-and-streaming`), and SHALL be evicted with them.

A sample outside resident data SHALL return a defined result — the coarsest resident level, or the
declared default — and SHALL NOT block or fault.

Fields SHALL support multiple resolutions, so a consumer that needs coarse data does not force fine
data resident.

#### Scenario: Empty regions cost nothing
- **WHEN** a field has data only near the surface
- **THEN** empty regions SHALL consume no storage

#### Scenario: Sampling unloaded data
- **WHEN** a system samples a field in an unloaded region
- **THEN** it SHALL receive the coarsest resident value or the declared default, without blocking

### Requirement: CPU and GPU access
Fields SHALL be samplable from both CPU code and GPU shaders, through interfaces that produce the
same value for the same position and resolution.

GPU access SHALL be through bindless resources reachable from the GPU scene, so a shader can sample
a field without per-draw binding.

CPU access SHALL be batchable, so a system sampling many positions does not pay per-sample
overhead.

#### Scenario: The same answer on both sides
- **WHEN** CPU foliage placement and a GPU terrain material sample the same field at the same point
- **THEN** they SHALL obtain the same value within the field's declared precision

#### Scenario: Bulk sampling
- **WHEN** a system samples a field at ten thousand positions
- **THEN** it SHALL be able to do so in one batched call

### Requirement: The wind field
Wind SHALL be a standard field with **layered contributions**: a prevailing wind, a
weather-driven contribution, local gusts, and transient sources such as explosions, vehicle wakes,
and rotor downwash.

The **producer** of the wind field is `weather-and-wind`, which composes prevailing wind from
climate, regional wind from weather cells and storms, terrain influence, and authored local volumes.
Transient sources are contributed by the systems that create them.

Every consumer of wind — foliage, cloth, VFX, water, clouds, and audio — SHALL sample this field.
No subsystem SHALL implement its own wind model.

Transient sources SHALL be registerable at runtime with a shape, strength, and lifetime, and SHALL
be bounded in number by a budget, with the lowest-priority sources dropped deterministically.

#### Scenario: Everything agrees about the wind
- **WHEN** trees, smoke, and water are visible together
- **THEN** they SHALL sample one wind field and move consistently

#### Scenario: An explosion moves everything nearby
- **WHEN** an explosion registers a transient wind source
- **THEN** foliage, particles, and cloth in range SHALL respond, with no per-system integration

#### Scenario: The field has a producer
- **WHEN** weather evolves
- **THEN** it SHALL write the wind field's weather-driven contribution, and consumers SHALL observe
  it without weather knowing they exist

### Requirement: Runtime field modification
Systems SHALL be able to write into fields they produce, at a declared granularity, and writes
SHALL be visible to consumers on a defined schedule rather than immediately mid-frame.

Runtime field changes SHALL be recorded in the world persistence overlay where the field declares
itself persistent, so a burned forest or a dried riverbed survives a save.

Modified regions SHALL raise **change events** so consumers can invalidate derived data
incrementally rather than polling.

#### Scenario: Fire changes the world
- **WHEN** fire spreads through a region
- **THEN** the burn-state field SHALL be updated, foliage and materials SHALL observe it, and the
  change SHALL persist if the field is declared persistent

#### Scenario: Consumers are notified
- **WHEN** a field region changes
- **THEN** a change event SHALL be raised so dependent data is invalidated for that region only

### Requirement: Determinism of gameplay-visible fields
A field whose values influence gameplay or simulation SHALL be declared **deterministic**: its
value at a position SHALL depend only on cooked data, the world seed, and recorded persistent
changes — never on frame timing, camera position, streaming order, or GPU execution order.

Fields used only for visual effect MAY be non-deterministic and SHALL be declared so.

A deterministic field SHALL NOT be produced by a GPU pass whose result is not read back
deterministically; such a field SHALL be either CPU-produced or declared visual.

#### Scenario: Flow drives a boat identically on two machines
- **WHEN** a deterministic flow field drives floating objects in a networked session
- **THEN** all peers SHALL observe the same values for the same simulation state

#### Scenario: Visual field is declared
- **WHEN** a field exists only to modulate a shader
- **THEN** it SHALL be declared visual, and gameplay SHALL be prevented from reading it by
  configuration validation

### Requirement: Field diagnostics
The engine SHALL provide, per field: a visualisation over the world, resident tile extents and
resolutions, memory in use, producer identity, sample cost, and a point query showing the value and
which layer contributed it.

#### Scenario: Why is this area dry
- **WHEN** vegetation is missing from a region
- **THEN** a developer SHALL be able to visualise the moisture field and query a point to see its
  value and its producer
### Requirement: Field residency levels
Fields SHALL support **residency levels** — macro, regional, and local — so that a world-scale field
can be present everywhere at low resolution while fine data exists only where it is needed.

Residency SHALL be requestable by streaming sources and by consumers, through the shared residency
policy in `residency`, and SHALL follow the same importance and budget model as other paged data.

A sample SHALL return the finest resident level at that position together with a **resolution
indicator**, so a consumer can decide whether the answer is precise enough rather than assuming it
is.

Macro-level data SHALL be resident for the whole world where a field declares it, since environmental
and ecosystem state must exist for regions that are not loaded.

#### Scenario: The whole world has weather
- **WHEN** a region is far from any viewer
- **THEN** its macro field values SHALL still exist and evolve, without fine tiles being resident

#### Scenario: Detail follows importance
- **WHEN** a region becomes important
- **THEN** finer field levels SHALL be requested through the residency policy like any other paged
  data

### Requirement: Field versioning and change events
Every field region SHALL carry a **version** that increments when its values change, and consumers
SHALL be able to detect staleness by comparing versions rather than by polling values.

Change events SHALL be raised at **region granularity** with the changed bounds, so that dependent
work — procedural generation, derived fields, cached results — can be invalidated precisely.

Versions SHALL participate in derivation keys, so that a cached result computed from a field is
invalidated when that field's region changes.

#### Scenario: A cached generation is invalidated
- **WHEN** moisture changes in one region
- **THEN** generated results derived from moisture there SHALL be invalidated, and results elsewhere
  SHALL remain cached

#### Scenario: Staleness is cheap to detect
- **WHEN** a consumer holds a derived value
- **THEN** it SHALL detect staleness by version comparison rather than by re-reading the field

### Requirement: Potential and current state
Fields describing what an environment **could support** SHALL be distinct from fields describing what
it **currently is**.

**Potential** — biome potential, vegetation potential — derives from slow inputs: climate, soil,
altitude, water availability. **Current state** — current biome, vegetation density, burn fraction —
reflects what events have left: fire, deforestation, drought, pollution, terraforming.

Current state SHALL evolve toward potential over time at a declared rate, and SHALL be reduced by
events, so that an environment recovers rather than being repainted.

Consumers SHALL be able to read either: procedural generation reads current state to materialise
detail, and reads potential to decide what recovery looks like.

#### Scenario: A burned forest is still forest country
- **WHEN** a forest burns
- **THEN** its current vegetation state SHALL drop while its potential remains, so it regrows as
  forest rather than becoming permanent grassland

#### Scenario: Terraforming raises potential first
- **WHEN** soil and moisture improve
- **THEN** potential SHALL rise, and current state SHALL follow at its declared rate
