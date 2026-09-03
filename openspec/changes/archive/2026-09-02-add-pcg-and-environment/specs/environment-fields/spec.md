## ADDED Requirements

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

## MODIFIED Requirements

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
