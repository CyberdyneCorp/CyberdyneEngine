# world-partition-and-streaming Specification

## Purpose

Defines **CyberWorld**: the spatial and persistence layer above the ECS, which decides what exists,
where it belongs, whether it should exist now, and where its persisted state lives.

It was the most-referenced missing capability in this specification set. Networking owned its own
replication cells, virtual geometry named content streaming as an unowned concept, and the GI scene
was specified as cell-scoped against a capability that did not exist. Those were four placeholders
for this.

The distinction that does the most work is that **cell residency, cell activation, asset residency,
and simulation detail are four separate axes**. Collapsing them is what makes crossing a boundary
mean *load everything now*; keeping them apart turns approach into a gradient where each step is
cheap because the expensive part already happened.

Two other decisions shape the rest. Positions are **cell-relative** — a cell coordinate plus a
32-bit local offset — which is smaller on the wire, stable under origin changes, directly
indexable, and precise at any distance, and which strengthens rather than reverses the engine's
policy of not going double-precision. And cells cook into **archetype blocks matching the runtime
chunk layout**, so activation is allocate, decompress, bulk copy — the direct dividend of having
built an archetype ECS.

Activation is transactional and incremental at once: preparation happens in private staging across
as many frames as it needs, and publication is atomic, so systems observe a cell as inactive or
fully active and never as half of one.

## Requirements

### Requirement: The persistent world is distinct from the ECS world
The engine SHALL distinguish two things and SHALL NOT name both "world" in its API:

- **`World`** — the ECS runtime container of entities, archetypes, and schedules, defined in
  `ecs-core`. It knows only what is currently instantiated.
- **The persistent world** — the spatial and persistence layer above it, addressed as
  `WorldAsset`, `WorldPartition`, `WorldStreaming`, and `WorldLayers`. It knows what exists, where
  it belongs, whether it should exist now, where its persisted state lives, and what assets it
  depends on.

The persistent world SHALL publish entities into the ECS world. The ECS world SHALL NOT call back
into the persistent world during activation.

The persistent world SHALL be decomposed into named subsystems — core identity and coordinates,
partition, streaming, layers, cooking, and editor tooling — rather than a single module.

#### Scenario: The distinction is visible in the API
- **WHEN** a developer reads the API
- **THEN** the ECS container SHALL be `World` and the spatial layer SHALL never be called `World`,
  so the two are not confused

#### Scenario: One direction only
- **WHEN** a cell is activated
- **THEN** entities SHALL be published into the ECS world, and no ECS system SHALL be required to
  know that the persistent world exists

### Requirement: The world asset is metadata, not content
A **world asset** SHALL contain metadata and indices: coordinate configuration, partition
settings, layer definitions, the cell index, references to global entities, and references to
scene instances.

It SHALL NOT contain the entities themselves. Entity data SHALL live in independent authoring
records and cooked cell packages.

Editing one object in a world SHALL NOT modify the world asset, so a single file does not become a
source-control bottleneck for every designer.

#### Scenario: One designer, one file
- **WHEN** a designer moves a single prop
- **THEN** only the authoring chunk containing it SHALL change, and the world asset SHALL be
  untouched

#### Scenario: World asset stays small
- **WHEN** a world contains millions of entities
- **THEN** the world asset SHALL remain an index whose size scales with cells and layers, not with
  entity count

### Requirement: World coordinates
The authoritative persistent form of a position SHALL be **cell-relative**: a cell coordinate plus
a 32-bit float local offset within that cell.

This SHALL be the form used for persistence, networking, and spatial indexing. A 64-bit accessor
SHALL be provided for tooling, geodetic work, and interchange, and SHALL NOT be the runtime
representation.

Because local coordinates are bounded by cell size, 32-bit precision SHALL be sufficient at any
distance from the world origin, consistent with the precision policy in `core-math`.

Conversion between cell-relative and simulation-local space SHALL be explicit, and the engine SHALL
define the origin used by physics, rendering, and audio for a given region.

#### Scenario: No precision loss at distance
- **WHEN** an entity is 1 000 km from the world origin
- **THEN** its position SHALL be exact in 32-bit local coordinates within its cell, without
  rebasing logic in gameplay code

#### Scenario: Cheap on the wire
- **WHEN** a position is replicated
- **THEN** the cell-relative form SHALL be used, rather than transmitting global double-precision
  coordinates

### Requirement: Partitioner interface
Spatial partitioning SHALL be a replaceable component implementing a **partitioner** interface, not
a fixed uniform grid baked into the world model.

The engine SHALL ship a **hierarchical grid** partitioner as the default, and SHALL admit uniform
grid, octree, and project-supplied partitioners.

The hierarchical grid SHALL provide multiple levels of decreasing cell size, configurable per
project, so that content of very different scales is not forced into one granularity.

#### Scenario: Custom partitioner
- **WHEN** a project supplies a partitioner for a planetary or non-Euclidean world
- **THEN** it SHALL integrate without changes to cell identity, streaming, or cooking

#### Scenario: Scales are not forced together
- **WHEN** a world contains both half-metre props and kilometre-scale terrain
- **THEN** the hierarchy SHALL allow each to be assigned at an appropriate level

### Requirement: Stable cell identity
Every cell SHALL have a **stable identifier** encoding its partition, hierarchy level, and spatial
position, generated deterministically from the partitioner's configuration.

The encoding SHALL be opaque to consumers.

Cell identifiers SHALL be stable across cooks with unchanged partition settings, because save
games, network protocol, patching, streaming caches, and build caches all key on them.

Changing partition settings SHALL be recognised as a change that invalidates cell identity, and
SHALL be reported as such rather than silently producing incompatible data.

#### Scenario: Save games survive a rebuild
- **WHEN** content changes but partition settings do not
- **THEN** cell identifiers SHALL be unchanged and existing saves SHALL remain valid

#### Scenario: Partition change is a breaking change
- **WHEN** cell size is changed
- **THEN** the build SHALL report that cell identity changes and what depends on it

### Requirement: Entity spatial binding and streaming policy
Every persistent entity SHALL carry a **spatial binding**: a home cell and a **streaming policy**.

| Policy | Meaning |
|---|---|
| `Spatial` | Exists when its cell is activated |
| `AlwaysLoaded` | Global; exists for the world's lifetime |
| `RuntimeManaged` | Created at runtime; lifetime owned by gameplay |
| `OwnerManaged` | Exists with its owner, not with a cell |
| `Transient` | Not persisted; never written to the world |

Assignment to a cell SHALL be derived from bounds, mobility, streaming policy, and importance —
**not from pivot position alone**, since a pivot says nothing about what an entity occupies.

#### Scenario: Global rules are not spatial
- **WHEN** a game-rules entity exists
- **THEN** it SHALL be `AlwaysLoaded` and SHALL NOT be assigned to a spatial cell

#### Scenario: Attached objects follow their owner
- **WHEN** a weapon is attached to a character
- **THEN** it SHALL be `OwnerManaged` and SHALL stream with the character rather than with a cell

#### Scenario: Bounds decide, not the pivot
- **WHEN** an entity's pivot lies in one cell but its bounds span several
- **THEN** assignment SHALL account for its bounds

### Requirement: Large entities occupy coarser levels
An entity whose bounds span many cells at one hierarchy level SHALL be assigned to a **coarser
level** whose cells contain it, rather than being duplicated into every overlapping cell.

The cooker SHALL select the level automatically from the entity's bounds, and SHALL report
entities forced to the coarsest level, since one such entity can pin a large region resident.

#### Scenario: A mountain is not duplicated
- **WHEN** a three-kilometre landform is partitioned
- **THEN** it SHALL be assigned to a macro-level cell, not replicated into hundreds of small ones

#### Scenario: Oversized content is reported
- **WHEN** an entity is too large for any reasonable level
- **THEN** the cook report SHALL name it, since it will keep a large region resident

### Requirement: Runtime cells carry subsystem payloads
A cooked cell SHALL be a **multi-subsystem package**, not a list of entities. It SHALL carry, as
independently streamable parts: entity data, an asset dependency set, and payloads for geometry,
physics, navigation, audio, illumination, and AI.

Payloads SHALL be separately requestable, so a consumer that needs only some of them does not pay
for the rest.

Bulk payload data SHALL be stored as content-addressed chunks through the asset system rather than
inline, so patching and deduplication work at the chunk level.

#### Scenario: Server does not load rendering payloads
- **WHEN** a dedicated server activates a cell
- **THEN** entity, physics, navigation, and AI payloads SHALL be loaded and geometry, texture, and
  audio payloads SHALL not be

#### Scenario: Patching is chunk-granular
- **WHEN** one material in a cell changes
- **THEN** only the affected chunks SHALL differ, and a patch SHALL not resend the cell

### Requirement: Cells are cooked in ECS-native form
Entity data in a cell SHALL be cooked as **archetype blocks**: for each archetype, an identifier
array and a column per component type, ready to be copied into ECS chunks.

Cooking SHALL NOT produce an entity-by-entity object graph requiring per-entity construction or
reflection at load.

Activation SHALL therefore be: allocate chunks, decompress, bulk copy, and fix up references.

#### Scenario: Activation is a copy
- **WHEN** a cell containing a hundred thousand entities is activated
- **THEN** its component data SHALL be bulk-copied into ECS chunks, without per-entity
  construction

#### Scenario: Layout matches the runtime
- **WHEN** a cell is cooked
- **THEN** its component columns SHALL match the runtime chunk layout, so the copy needs no
  transformation

### Requirement: Residency and activation are distinct
A cell SHALL have a state richer than loaded or unloaded:

| State | Meaning |
|---|---|
| `Unloaded` | Nothing but its index entry |
| `Metadata` | Bounds, cost, and dependencies known |
| `Prefetching` | Data and assets in flight |
| `Resident` | Data and resources in memory; entities not yet in the ECS world |
| `Activated` | Entities published and participating in simulation |
| `Deactivating` | Being withdrawn |
| `Evictable` | Resident but not needed; memory reclaimable |

**Residency and activation SHALL be independently controllable.** A cell may be resident without
being activated, so that approaching content is prepared long before it is instantiated.

Asset residency and simulation detail SHALL be separate axes again, owned by the asset system and
by the AI, animation, and physics LOD systems respectively. Collapsing these four axes SHALL NOT
occur.

#### Scenario: Approach is a gradient
- **WHEN** a player approaches a city over two kilometres
- **THEN** metadata, coarse geometry residency, asset prefetch, and entity activation SHALL occur
  at different distances rather than at one boundary

#### Scenario: Resident but inactive
- **WHEN** a cell is resident and not activated
- **THEN** its entities SHALL not exist in the ECS world and SHALL cost no simulation time

### Requirement: Streaming sources
Streaming SHALL be driven by explicit **streaming sources**, each with a location, a shape, a
radius or extent, a priority, a prediction horizon, and a **channel mask**.

Sources SHALL include at minimum: players, cameras, editor viewports, network clients, teleport
destinations, cinematic cameras, mission targets, and AI groups. Any subsystem SHALL be able to
register one.

Multiple sources SHALL combine: a cell required by any source is required.

#### Scenario: The camera is not the only source
- **WHEN** a strategy camera is far from the units it commands
- **THEN** both the camera and the units SHALL be able to register sources, and content around
  both SHALL stream

#### Scenario: Teleport destination
- **WHEN** a teleport is initiated
- **THEN** a high-priority source at the destination SHALL cause its content to stream before
  arrival

### Requirement: Streaming shapes and prediction
A streaming source SHALL support shapes beyond a radius: sphere, box, frustum, cone, path, and
spline.

Sources SHALL support **prediction**: the planner SHALL extrapolate from velocity, from a
navigation or spline path, and from declared future positions such as a cinematic's camera track,
and SHALL request content before it is needed.

Streaming priority SHALL combine source importance, predicted visibility, estimated time until
needed, and gameplay importance, and SHALL be computed centrally rather than by each source.

#### Scenario: A vehicle streams along its route
- **WHEN** a vehicle follows a path at speed
- **THEN** cells along the predicted path SHALL be requested ahead of arrival rather than as they
  are entered

#### Scenario: Frustum matters more than position
- **WHEN** a strategy camera looks across a battlefield
- **THEN** a frustum-shaped source SHALL prioritise what is visible over what is nearby

### Requirement: Streaming channels
Streaming SHALL be **per channel**, not all-or-nothing. Channels SHALL include at minimum:
entities, geometry, textures, physics, navigation, AI, audio, and illumination.

A streaming source SHALL declare which channels it requires, and a cell SHALL stream only the
channels some active source requires.

#### Scenario: Spectator does not need physics
- **WHEN** a remote spectator camera streams content
- **THEN** geometry and textures SHALL stream and physics, navigation, and AI SHALL not

#### Scenario: Channel is added later
- **WHEN** a source's channel mask gains physics
- **THEN** the physics payload SHALL stream for the cells already resident, without reloading them

### Requirement: Streaming priority, deadlines, and budgets
Every streaming request SHALL carry a priority, a desired state, and a **deadline**, so that an
urgent request and a background prefetch are distinguishable.

The system SHALL hold explicit budgets: I/O bandwidth, entity memory, and a per-frame **activation
time budget**, and SHALL coordinate with the asset system's residency budget rather than
maintaining a competing one.

When work exceeds budget, it SHALL be ordered: critical (teleport, gameplay-blocking), then
gameplay-relevant, then visible, then predicted, then background.

The activation budget SHALL be held by spreading preparation across frames, never by activating a
cell partially.

#### Scenario: Urgent beats predicted
- **WHEN** a teleport request and a background prefetch compete for I/O
- **THEN** the teleport SHALL be serviced first and the prefetch deferred

#### Scenario: Activation does not spike
- **WHEN** a large cell becomes ready
- **THEN** its preparation SHALL be spread across frames within the activation budget, and the
  frame SHALL not stall

#### Scenario: Budget exhaustion is reported
- **WHEN** requested content exceeds the memory budget
- **THEN** the lowest-priority resident cells SHALL be evicted and the shortfall SHALL be reported

### Requirement: Activation is staged and published atomically
Cell activation SHALL prepare in **private staging** — allocating chunks, decoding component
blocks, building physics batches, uploading GPU scene data, resolving intra-cell references —
without any of it being observable, and SHALL then **publish atomically**.

Systems SHALL observe a cell as either inactive or fully activated, never partially.

Preparation MAY span multiple frames. During it, the cell's world HLOD representation SHALL remain
visible, so the transition is not visible as absence.

Deactivation SHALL be the same in reverse: withdraw atomically, then release incrementally.

#### Scenario: No half-activated cell
- **WHEN** a system queries during a cell's preparation
- **THEN** it SHALL observe the cell as inactive, and SHALL NOT see some of its entities

#### Scenario: Preparation is amortised
- **WHEN** a cell needs more preparation than one frame's budget allows
- **THEN** preparation SHALL continue across frames with the HLOD proxy visible, and publication
  SHALL occur when it completes

#### Scenario: Batch registration
- **WHEN** a cell's collision is registered with physics
- **THEN** bodies SHALL be added in bulk where the backend supports it, not one at a time

### Requirement: Cell lifecycle events
Cell state transitions SHALL be published as **structured events** — resident, activated,
deactivated, evicted — into subsystem queues, consumed at defined points.

Streaming SHALL NOT invoke arbitrary subsystem callbacks during activation, since re-entrant
callbacks during streaming are a reliable source of ordering bugs.

Consumers SHALL be able to declare ordering requirements between themselves for a given
transition.

#### Scenario: Subsystems react in order
- **WHEN** a cell is activated
- **THEN** navigation, audio, illumination, and gameplay SHALL consume the event at their own
  defined points, in a declared order

#### Scenario: No re-entrancy
- **WHEN** a consumer reacts to activation by requesting another cell
- **THEN** the request SHALL be queued, not processed re-entrantly within the activation

### Requirement: Persistent entity identity
Persistent entities SHALL carry a **persistent identifier** that is stable across editing,
cooking, saving, networking, and world reload, and that is independent of runtime entity
identifiers and of which file the entity is stored in.

Runtime entity identifiers SHALL remain small and fast as specified in `ecs-core`. The persistent
identifier SHALL be resolved through a registry, and SHALL NOT be used in hot loops.

Persistent identifiers SHALL be assigned at authoring time, and SHALL NOT be derived from
position, file, index, or path, so that moving an entity does not change its identity.

Duplicate persistent identifiers SHALL be a cook error.

#### Scenario: Identity survives reorganisation
- **WHEN** an entity is moved between authoring chunks or its prefab is edited
- **THEN** its persistent identifier SHALL be unchanged, and references to it SHALL still resolve

#### Scenario: Hot loops use runtime identifiers
- **WHEN** a system iterates entities
- **THEN** it SHALL use runtime entity identifiers, not persistent ones

### Requirement: Cross-cell references
References between persistent entities SHALL be **persistent references**, valid whether or not the
target is loaded. A raw pointer or runtime entity identifier SHALL NOT be persisted.

Each reference SHALL declare a **policy**:

| Policy | Meaning |
|---|---|
| `Soft` | May be unresolved; the holder handles absence |
| `LoadOnDemand` | Resolving it issues a streaming request |
| `RequireLoaded` | The target must be resident whenever the holder is |
| `FollowOwner` | The target lives with its owner rather than a cell |

Resolution SHALL return either a runtime entity or a defined unresolved result. It SHALL NOT block
the caller.

#### Scenario: Reference to unloaded content
- **WHEN** a quest references an entity in an unloaded region
- **THEN** the reference SHALL remain valid and resolve to an unresolved result, not a dangling
  handle

#### Scenario: On-demand resolution
- **WHEN** a `LoadOnDemand` reference is resolved
- **THEN** a streaming request SHALL be issued and the caller SHALL be able to continue without
  blocking

### Requirement: Dependency explosion detection
The cooker SHALL compute the **transitive closure of hard cell dependencies** created by
`RequireLoaded` references and by always-loaded content, and SHALL report, per cell, how many cells
and how many bytes its activation forces resident.

Thresholds SHALL be configurable, and exceeding them SHALL fail the build or warn, by
configuration.

The report SHALL name the reference chain responsible, so the cause is actionable rather than a
number.

#### Scenario: Walking into one cell loads a continent
- **WHEN** hard references chain across many cells
- **THEN** the cook report SHALL state that activating the first pulls the rest, with the chain
  that caused it

#### Scenario: Threshold enforced
- **WHEN** a cell's hard dependency closure exceeds the configured limit
- **THEN** the build SHALL fail or warn as configured, rather than shipping the problem

### Requirement: World layers
The world SHALL support **layers**: named groupings of content orthogonal to spatial partitioning.
A cell may contain entities belonging to several layers, and a layer may span many cells.

Layer kinds SHALL include at minimum: editor-only, runtime, scenario, variant, and system layers.

Layers SHALL be identified by **stable identifiers**, not by name or path, so a layer can be
renamed without rewriting the entities that belong to it.

Editor-only layers SHALL NOT be cooked into runtime data.

#### Scenario: Layers cut across cells
- **WHEN** a seasonal variant spans a whole region
- **THEN** it SHALL be a layer, and its entities SHALL still be partitioned into the cells they
  occupy

#### Scenario: Renaming is safe
- **WHEN** a layer is renamed
- **THEN** entity membership SHALL be unaffected, since membership keys on the identifier

### Requirement: Layer states and scenario switching
A runtime layer SHALL have states: `Unloaded`, `Loaded`, and `Activated`, controllable
independently of cell streaming, so a layer can be made resident before being activated.

Activating or deactivating a layer SHALL be a single operation affecting all its entities, rather
than a per-entity change.

Layer state SHALL be part of the persistence overlay and SHALL be replicable as an identifier plus
a state, not as per-entity messages.

Entities not belonging to a switched layer SHALL be unaffected, so gameplay state established
before a scenario change survives it.

#### Scenario: Scenario switch is one operation
- **WHEN** a mission destroys a city
- **THEN** the destroyed-city layer SHALL be activated and the intact layer deactivated as one
  operation, not twenty thousand property changes

#### Scenario: Prepared before switched
- **WHEN** a scenario layer is expected soon
- **THEN** it SHALL be loaded in advance and activated instantly when the event occurs

#### Scenario: Replication is cheap
- **WHEN** the server changes a layer's state
- **THEN** it SHALL replicate the layer identifier and state, not the individual entity changes

### Requirement: World hierarchical level of detail
The world SHALL support **world HLOD**: aggregate representations standing in for the content of
one or more cells at distance, so that distant regions require neither entities nor their assets.

World HLOD SHALL be distinguished from geometric level of detail: virtual geometry reduces triangle
detail **within** an object; world HLOD replaces **many objects** with one aggregate. Both SHALL
exist and SHALL NOT be conflated.

HLOD proxies SHALL be generated at cook time by aggregating static content, and MAY themselves be
virtual geometry assets.

The transition between an HLOD proxy and its real cells SHALL be driven by the same streaming
state machine, and the proxy SHALL remain visible until the cells are published.

#### Scenario: A district is one object
- **WHEN** two thousand buildings are viewed from far away
- **THEN** an aggregate proxy SHALL be rendered and neither their entities nor their assets SHALL
  be resident

#### Scenario: The swap is not visible
- **WHEN** cells behind a proxy finish activating
- **THEN** the proxy SHALL be replaced in the same frame the cells are published

### Requirement: Dynamic entity migration
An entity's **home cell** — where it is persisted — SHALL be distinct from the **runtime spatial
cell** it currently occupies.

Moving entities SHALL be tracked in a dynamic spatial index while active, and crossing a cell
boundary SHALL NOT rewrite persistent ownership.

Persistent position SHALL be updated at defined checkpoints — save, deactivation, or explicit
request — into the persistence overlay, not continuously.

An entity whose runtime cell deactivates while it remains relevant SHALL be handled by policy:
migrating to an active cell, becoming runtime-managed, or being persisted and removed.

#### Scenario: A vehicle crosses a hundred cells
- **WHEN** a vehicle drives across a large world
- **THEN** it SHALL be tracked in the dynamic index without rewriting its home cell each boundary

#### Scenario: Its region unloads
- **WHEN** the cell an active entity occupies is deactivated
- **THEN** the declared policy SHALL apply, and the entity SHALL NOT be silently destroyed

### Requirement: Persistence overlay
Runtime changes to the world SHALL be recorded in a **persistence overlay**, and cooked cells SHALL
remain immutable:

```
authored cells + persistence overlay = current world
```

The overlay SHALL record: entities created and removed, component values changed, layer states,
dynamic entity positions at checkpoints, and world state variables.

One overlay mechanism SHALL serve save games, dedicated server persistence, replays, and the
editor's play-mode changes. Its **encoding, journalling, atomicity, incremental writing, migration,
and storage backends** are defined in `save-and-persistence`; this capability defines the model the
world maintains.

The overlay SHALL be organised so that the persistent state of **unloaded regions** is available
without loading them, so that saving a world of which most is unloaded requires no additional
streaming.

Applying an overlay to authored cells SHALL be deterministic, and SHALL occur **during cell
activation** rather than by instantiating authored content and then correcting it.

An overlay SHALL declare the content version it was produced against so incompatibility is detected
rather than misapplied.

#### Scenario: A destroyed building stays destroyed
- **WHEN** a building is destroyed and the game is saved and reloaded
- **THEN** the authored cell SHALL be loaded unchanged and the overlay SHALL remove the building

#### Scenario: Content update after a save
- **WHEN** a save's content version does not match the installed content
- **THEN** the mismatch SHALL be detected and reported, not silently applied

#### Scenario: Saving does not stream the world
- **WHEN** a world with most regions unloaded is saved
- **THEN** the persistent state of unloaded regions SHALL be available without loading them

### Requirement: Cell cost model
Cooking SHALL produce a **cost estimate** per cell: compressed I/O bytes, CPU memory, GPU memory,
entity count, physics body count, navigation tile count, and estimated activation time.

The streaming planner SHALL use these to decide what to request and what to evict, rather than
discovering cost after loading.

Estimates SHALL be validated against measured runtime cost, and significant divergence SHALL be
reported so the model does not silently drift.

#### Scenario: Planning uses known cost
- **WHEN** two candidate cells compete for a budget
- **THEN** the planner SHALL compare their estimated costs before requesting either

#### Scenario: Estimates are checked
- **WHEN** a cell's measured activation time greatly exceeds its estimate
- **THEN** the divergence SHALL be reported so the cost model can be corrected

### Requirement: Client and server world profiles
The client and the dedicated server SHALL use **one world definition with different profiles**, not
two world systems.

A profile SHALL select which channels are cooked and streamed and which subsystem payloads are
required. A server profile SHALL typically require entities, physics, navigation, AI, and network
metadata, and omit geometry, textures, audio, and illumination.

Cell identity, entity identity, and layer identity SHALL be identical across profiles, so client
and server refer to the same content.

#### Scenario: Server build omits rendering data
- **WHEN** a dedicated server build is cooked
- **THEN** rendering payloads SHALL be omitted and cell and entity identity SHALL be unchanged

#### Scenario: One codebase
- **WHEN** streaming behaviour is changed
- **THEN** it SHALL change for client and server alike, since they are profiles of one system

### Requirement: Representation tiers
The world SHALL support **representation tiers** for content that must exist without being fully
instantiated:

| Tier | Meaning |
|---|---|
| 0 | Full entities |
| 1 | Aggregate: one entity representing a group |
| 2 | Statistical: state without entities |

The world SHALL own **promotion and demotion** between tiers as streaming state changes, including
materialising individuals from an aggregate and collapsing individuals back into one.

The division of responsibility SHALL be explicit: the world owns *whether content exists and in
what form*; `ai-system` owns *how much simulation that form receives*.

Promotion and demotion SHALL preserve identity and gameplay-relevant state, so an army that is
demoted and later promoted has not silently changed.

#### Scenario: A distant army exists without entities
- **WHEN** a hostile force is far from any streaming source
- **THEN** it SHALL exist as an aggregate with position, strength, and destination, and its
  individual units SHALL not be instantiated

#### Scenario: Materialisation preserves state
- **WHEN** a player approaches that force
- **THEN** individuals SHALL be materialised consistently with the aggregate's state, not
  regenerated arbitrarily

### Requirement: Subsystem integration contracts
The world SHALL define the contract by which each subsystem consumes cell lifecycle:

| Subsystem | Contract |
|---|---|
| Rendering | Cell activation publishes instances into the GPU scene; virtual geometry streams detail separately |
| Physics | Resident cells may preload collision; activation registers bodies in bulk |
| Navigation | Cell events drive tile residency; navigation owns its own tile layout, which need not match cells |
| Illumination | Cells carry GI payloads ingested by the GI scene, and evicted on unload |
| Audio | Cells carry ambient zones, reverb metadata, and acoustic geometry |
| Networking | Replication cells derive from the world partition |
| AI | Cell events drive representation tier changes |
| Environment fields | Cells carry field tiles, ingested on residency and evicted on unload (see `environment-fields`) |
| Terrain | Cells carry terrain tile data, collision, and navigation contribution as separate channels (see `terrain`) |
| Foliage | Cells carry foliage clusters and placement rule bindings; instances publish into the GPU scene (see `foliage`) |
| Water | Cells carry water body **segments**; the body's identity and network are global while its runtime data is segmented (see `water`) |

Subsystems SHALL own their internal granularity. The world SHALL NOT impose its cell boundaries on
a subsystem whose optimal partition differs.

A subsystem whose logical object spans many cells — a river, an ocean, a terrain — SHALL be
represented as one logical entity with segmented runtime data, rather than as unrelated per-cell
objects.

#### Scenario: Navigation keeps its own tiling
- **WHEN** a cell activates
- **THEN** navigation SHALL update its own tiles, which need not align with cell boundaries

#### Scenario: Two scales of streaming
- **WHEN** a region streams in
- **THEN** the world SHALL establish which objects exist, and virtual geometry and texture
  streaming SHALL determine their detail independently

#### Scenario: A river is one thing
- **WHEN** a river crosses hundreds of cells
- **THEN** it SHALL be one water body with segmented runtime data, not hundreds of independent
  water objects

### Requirement: World cooking pipeline
World cooking SHALL: resolve the authoring graph including prefab and scene instances, apply
variants and overrides, validate references, flatten hierarchy where runtime relationships are
unnecessary, assign persistent identities, partition spatially, build archetype blocks, build
subsystem payloads, extract asset dependencies, compute the cell dependency graph and cost model,
and content-address the result.

The **authoring partition SHALL be independent of the runtime partition**. Changing runtime cell
size SHALL be a cook setting and SHALL NOT require designers to reorganise authoring data.

Cooking SHALL be **incremental**: cook keys SHALL include authoring chunk content, prefab and scene
content, dependency hashes, partitioner configuration, and cooker version, so a local change
rebuilds only affected cells.

#### Scenario: One material change does not rebuild the world
- **WHEN** a single material is edited
- **THEN** only cells depending on it SHALL be recooked

#### Scenario: Cell size is a cook setting
- **WHEN** runtime cell size is changed
- **THEN** the world SHALL be repartitioned by the cooker, with no change to authoring data

#### Scenario: Patches are small
- **WHEN** a content patch is produced
- **THEN** only changed content-addressed chunks SHALL be included

### Requirement: World validation
Validation SHALL run at cook time and in the editor, and SHALL detect: duplicate persistent
identifiers, broken references, hard cross-cell dependency explosions, entities too large for any
partition level, always-loaded content that is not streamable, prefab and scene dependency cycles,
conflicting layer configurations, missing schema migrations, and orphaned prefab overrides.

Each SHALL be classified as an error or a warning by configuration, and errors SHALL fail
continuous integration.

Diagnostics SHALL name the responsible entity, asset, or reference chain.

#### Scenario: Broken reference fails the build
- **WHEN** a persistent reference targets a deleted entity
- **THEN** validation SHALL report it with the holder and the target, and fail as configured

#### Scenario: Editor sees the same problems
- **WHEN** a designer creates a problem
- **THEN** it SHALL be reported in the editor, not only at cook time

### Requirement: Streaming diagnostics
The engine SHALL provide a **streaming debugger** showing: cell bounds and states, streaming
sources and their shapes, per-cell priority, cost and residency, layer states, cross-cell
references, HLOD state, and pending requests.

For any cell it SHALL answer **why it is in its current state**: which sources require it, its
computed priority, its predicted relevance, and what is blocking a transition.

A profiler view SHALL report: cell counts by state, memory by category, I/O throughput and queue
depth, activation time against budget, and evictions with their cause.

#### Scenario: Why is this loaded
- **WHEN** a developer selects a resident cell
- **THEN** the debugger SHALL name the streaming sources requiring it and the priority that caused
  it to be requested

#### Scenario: Why has this not loaded
- **WHEN** expected content is missing
- **THEN** the debugger SHALL state whether it is unrequested, queued behind higher priority,
  budget-blocked, or failed

#### Scenario: Memory is attributable
- **WHEN** world memory is high
- **THEN** the profiler SHALL attribute it to cells, categories, and layers

### Requirement: World gameplay API
Gameplay SHALL address the world through a small interface: spawning from a prefab with parameters,
resolving persistent references, requesting a prefetch around a location with a priority, and
activating or deactivating layers.

Cell identifiers, partition structure, and streaming internals SHALL NOT appear in the ordinary
gameplay API; a low-level interface MAY expose them for tools and advanced use.

The C ABI and the Swift overlay SHALL expose the same operations, with the Swift form using
structured concurrency for prefetch completion.

#### Scenario: Prefetch before a teleport
- **WHEN** gameplay is about to teleport a player
- **THEN** it SHALL request a prefetch around the destination and await it, without referring to
  cells

#### Scenario: Resolving a reference
- **WHEN** gameplay resolves a persistent reference whose target is unloaded
- **THEN** it SHALL receive a defined unresolved result rather than blocking or faulting
