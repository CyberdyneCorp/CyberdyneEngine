## ADDED Requirements

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
