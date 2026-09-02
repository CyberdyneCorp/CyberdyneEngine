## MODIFIED Requirements

### Requirement: Memory pressure levels
The engine SHALL maintain a **pressure level** — `Normal`, `Elevated`, or `Critical` — derived from
budget utilisation and from platform-reported memory conditions, and SHALL broadcast changes to
subsystems.

Subsystems SHALL declare their response:

| Level | Expected response |
|---|---|
| `Normal` | Prefetch and cache freely within budget |
| `Elevated` | Trim caches, reduce prefetch distance, evict unreferenced data |
| `Critical` | Drop optional caches, force streaming quality down, defer non-essential work |

Pressure SHALL be **the coordination mechanism** for memory across streaming, geometry, texture,
shadow, world, audio, and asset residency, in the same way the renderer budget arbiter coordinates
GPU time. Subsystems SHALL respond to the declared level rather than each polling platform memory.

For **paged subsystems** — virtual geometry, virtual textures, virtual shadows, and illumination
caches — the response SHALL be coordinated by the residency layer (see `residency`), which weighs
their reductions against each other by importance and visible impact rather than letting each evict
independently.

GPU memory SHALL participate in the same pressure model as CPU memory.

Pressure transitions SHALL be hysteretic, so systems do not oscillate between trimming and
refilling.

An allocation failure SHALL remain a defined outcome — the allocator returns null and the caller
surfaces an error — but pressure SHALL be the mechanism that prevents it, since by the time an
allocation fails the system has already failed.

#### Scenario: Everything trims together
- **WHEN** pressure reaches `Elevated`
- **THEN** every cache-holding subsystem SHALL trim according to its declared response, rather than
  one subsystem freeing memory that another immediately consumes

#### Scenario: No oscillation
- **WHEN** utilisation hovers at a threshold
- **THEN** hysteresis SHALL prevent repeated trim-and-refill cycles

#### Scenario: Pressure precedes failure
- **WHEN** memory is exhausted
- **THEN** `Critical` pressure SHALL have been signalled beforehand and recorded, so the failure is
  diagnosable

#### Scenario: Paged subsystems reduce together
- **WHEN** pressure forces a reduction across geometry, texture, and shadow caches
- **THEN** the residency layer SHALL decide the split by importance, rather than three independent
  evictions competing
