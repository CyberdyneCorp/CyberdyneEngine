## ADDED Requirements

### Requirement: A save is the overlay, not a second model
A save SHALL be the **world persistence overlay** (see `world-partition-and-streaming`) together
with scoped state fragments, encoded for durability.

```
authored content + persistence overlay + scoped fragments = saved game
```

A second persistence model SHALL NOT exist. Two models would be two sources of truth about whether
a bridge is destroyed.

Cooked content SHALL never be rewritten by a save.

#### Scenario: One truth about the world
- **WHEN** a destroyed structure is saved
- **THEN** it SHALL be recorded in the overlay that the world already maintains, not in a parallel
  representation

#### Scenario: Content is untouched
- **WHEN** a game is saved
- **THEN** no cooked content SHALL be modified

### Requirement: Persistence scopes
Saved state SHALL be organised into **scopes** with distinct lifetimes and distinct storage:
profile, game instance, campaign, session, world, and participant.

A monolithic save blob SHALL NOT be produced. Graphics settings, unlocked achievements, campaign
world state, and match state have different lifetimes, different cloud semantics, and different
sharing rules.

Each scope SHALL be independently loadable, so that a profile can be read without a campaign, and a
campaign without a session.

#### Scenario: Settings survive a deleted campaign
- **WHEN** a campaign save is deleted
- **THEN** profile settings, progression, and unlocks SHALL be unaffected

#### Scenario: Scopes load independently
- **WHEN** a menu needs progression to display
- **THEN** the profile scope SHALL be readable without loading world state

### Requirement: Persistence traits
Fields SHALL declare **persistence traits**, extending the field classification already defined in
`serialization-and-prefabs`: authoring, save game, profile, replicated, replay relevant, runtime
only, and derived. A field MAY carry several.

Traits SHALL determine what a save captures, and SHALL be the **only** mechanism doing so — a save
SHALL NOT be assembled by a hand-maintained list of what to write.

**Derived state SHALL NOT be saved**: spatial indexes, cached transforms, GPU scene contents,
residency state, shadow pages, navigation working data, temporal history, and particle state are
reconstructed after load.

#### Scenario: The schema decides
- **WHEN** a component gains a field
- **THEN** whether it is saved SHALL follow from its declared traits with no save code change

#### Scenario: Caches are rebuilt
- **WHEN** a save is loaded
- **THEN** derived data SHALL be reconstructed rather than restored

### Requirement: Persistent identity
Saved references SHALL use **stable identities** — persistent entity identity, asset identity,
participant, team, type, and field identifiers — as required by `core-type-system` and
`world-partition-and-streaming`.

Runtime entity indices, memory addresses, pointers, and archetype positions SHALL NEVER be
serialised.

A reference whose target is not resident SHALL remain valid and resolve to a defined unresolved
state, not become invalid.

#### Scenario: A save survives repartitioning
- **WHEN** content is rebuilt with unchanged partition settings
- **THEN** saved references SHALL resolve to the same objects

#### Scenario: References into unloaded regions
- **WHEN** a quest refers to an entity in an unloaded region
- **THEN** the reference SHALL be preserved and resolvable when that region loads

### Requirement: Entity deltas and tombstones
Persistent entity state SHALL be recorded as **deltas against authored content**:

| State | Recorded |
|---|---|
| Unchanged authored entity | Nothing |
| Modified authored entity | Its changed persistent fields |
| Destroyed authored entity | A **tombstone** |
| Runtime-created entity | Its template, spawn transform, ownership, and persistent overrides |

An entity that has not changed SHALL contribute nothing to the save.

On load, authored content SHALL be instantiated and the delta applied: tombstoned entities SHALL NOT
be instantiated, modified entities SHALL receive their changes, and created entities SHALL be
spawned from their templates.

#### Scenario: A destroyed bridge stays destroyed
- **WHEN** an authored bridge was destroyed and the game is reloaded
- **THEN** the authored cell SHALL load unchanged and the tombstone SHALL prevent instantiation

#### Scenario: A built structure returns
- **WHEN** a player-built structure is saved
- **THEN** it SHALL be recorded as a template, transform, ownership, and persistent overrides, not
  as a dump of its runtime components

### Requirement: Dirty tracking
Writing a field with a persistence trait SHALL mark its record **persistent-dirty**, and saving
SHALL inspect dirty records rather than scanning the world.

An autosave in a world of ten million persistent objects SHALL cost work proportional to what
changed, not to the size of the world.

Dirty state SHALL survive streaming: an entity modified and then unloaded SHALL still contribute its
change to the next save.

#### Scenario: An autosave does not walk the world
- **WHEN** an autosave runs after a few thousand changes in a very large world
- **THEN** it SHALL process those changes, not every entity

#### Scenario: Changes survive unloading
- **WHEN** a modified region unloads before the next save
- **THEN** its persistent changes SHALL still be saved

### Requirement: Saving an unloaded world
Producing a save SHALL NOT require loading regions that are not resident.

Persistence for unloaded regions SHALL be maintained in a **persistent state store** organised by
world region, so that the current persistent state of a distant destroyed village exists
independently of whether it is loaded.

Saving SHALL serialise the store's contents for unloaded regions and the live overlay for resident
ones, and the result SHALL be identical either way for the same logical state.

#### Scenario: A save with five per cent resident
- **WHEN** a world with most regions unloaded is saved
- **THEN** no additional region SHALL be loaded to produce the save

#### Scenario: Residency does not change the result
- **WHEN** the same logical state is saved with different regions resident
- **THEN** the saved state SHALL be equivalent

### Requirement: The save journal
Persistent change MAY be recorded incrementally in a **journal** appended as change occurs, with a
base checkpoint periodically **compacted** from base plus journal.

Journalling SHALL allow frequent, cheap capture without rewriting a full save, and SHALL be the
mechanism shared with the editor's transaction journal and crash recovery where a project uses it.

Compaction SHALL be atomic and SHALL leave the previous base valid until it completes.

#### Scenario: Frequent capture is cheap
- **WHEN** checkpoints are taken often
- **THEN** each SHALL append changes rather than rewriting the full state

#### Scenario: Compaction is safe
- **WHEN** compaction is interrupted
- **THEN** the previous base and journal SHALL remain loadable

### Requirement: Consistent snapshots and background writing
A save SHALL be taken from a **tick-consistent** view of authoritative state captured at a commit
boundary, and serialisation, compression, and writing SHALL proceed in the background while
simulation continues.

The main-thread cost of capturing that view SHALL be **bounded and budgeted**, and SHALL be achieved
by exploiting chunked storage — versioning or copy-on-write of the chunks that hold persistent state
— rather than by copying the world.

Presentation and derived data SHALL NOT be captured.

The simulation SHALL NOT be paused for the duration of a save.

#### Scenario: Saving does not stall the game
- **WHEN** a large save is produced
- **THEN** the capture SHALL be a bounded main-thread cost and the remainder SHALL happen in the
  background

#### Scenario: The snapshot is one moment
- **WHEN** state is captured
- **THEN** it SHALL correspond to one commit boundary, not to values read at different times

### Requirement: Save container and manifest
A save SHALL have a **manifest** declaring: save format version, build identity, project identity,
save and campaign identity, the simulation point, the session seed, content and plugin manifest
hashes, scope inventory, and a chunk index.

Content SHALL be **chunked**, so that loading does not require reading the whole save into memory
and so that incremental writes touch only changed chunks.

Save format version and type schema versions SHALL be **independent**: a container change and a
gameplay type change are different events and SHALL NOT share one number.

#### Scenario: A large save loads incrementally
- **WHEN** a multi-gigabyte save is loaded
- **THEN** chunks SHALL be read as needed rather than the whole file at once

#### Scenario: Versions are separate
- **WHEN** a gameplay type's schema changes
- **THEN** the save format version SHALL be unaffected

### Requirement: Atomic writes and generations
Saving SHALL be **atomic**: write new chunks, verify them, write the new manifest, and switch, with
durability applied appropriately for the platform.

**Existing save data SHALL NEVER be overwritten in place.** A crash, power loss, or storage error at
any point SHALL leave the previous save valid and loadable.

Multiple **generations** SHALL be retained by policy, with the manifest determining the active one,
so that a corrupted newest save falls back rather than losing progress.

#### Scenario: A crash mid-save loses nothing
- **WHEN** the process is killed while saving
- **THEN** the previous generation SHALL remain valid and loadable

#### Scenario: A corrupt newest generation
- **WHEN** the newest generation fails verification
- **THEN** an earlier generation SHALL be loadable and the failure reported

### Requirement: The load pipeline
Loading SHALL proceed: read the manifest, validate compatibility, load profile and session scopes,
construct the session, load the persistent world index, begin world streaming, **apply persistent
deltas as cells activate**, restore participant and control state, and resume simulation.

The entire world SHALL NOT be instantiated before the player sees anything; loading SHALL use the
same streaming path as normal play.

Delta application SHALL occur during cell activation, so a region's authored content and its
persistent changes are combined once rather than instantiated and then corrected.

#### Scenario: Loading streams
- **WHEN** a save in a large world is loaded
- **THEN** initial regions SHALL stream in and play SHALL begin without instantiating the whole
  world

#### Scenario: Deltas apply on activation
- **WHEN** a region activates during load
- **THEN** its authored content and persistent deltas SHALL be combined during activation

### Requirement: Compatibility and migration
A save SHALL declare its compatibility requirements, and a project SHALL declare its policy: exact
build, same major version, migratable, or best effort.

Migration SHALL use the **value-record mechanism** defined in `serialization-and-prefabs`, operating
on identifiers rather than on obsolete native types, and SHALL apply to save data as it does to
authoring data.

Load failures SHALL be **structured**: incompatible build, migration failed, missing plugin,
corrupt chunk, missing content, unresolvable reference. A boolean failure SHALL NOT be the interface.

**Cooked content SHALL NOT be migrated at runtime**; a save referencing incompatible content SHALL
be rejected with a reason.

#### Scenario: A campaign survives an update
- **WHEN** a game updates and a type's schema changes
- **THEN** existing saves SHALL migrate through the registered chain, or be rejected with a reason

#### Scenario: Failures are diagnosable
- **WHEN** a save fails to load
- **THEN** the failure SHALL name what was incompatible, missing, or damaged

### Requirement: Plugin-owned state
Saved records SHALL identify the **owning module or plugin** of the types they contain.

Loading a save whose required plugin is absent SHALL produce a structured error naming the plugin
and version, not a partial load.

Where a plugin is optional and policy permits, its state MAY be **preserved opaquely** so that
re-enabling it restores that state rather than having lost it.

#### Scenario: A missing plugin is named
- **WHEN** a save requires an absent plugin
- **THEN** the error SHALL name the plugin and its version rather than reporting corruption

#### Scenario: Optional state survives
- **WHEN** an optional plugin is disabled and later re-enabled
- **THEN** its preserved state SHALL be restored if policy allowed preservation

### Requirement: Integrity and confidentiality
Save chunks SHALL carry **content hashes**, and the manifest SHALL carry the hashes of its chunks,
so corruption is detected rather than loaded.

Where confidentiality is required, established authenticated encryption SHALL be used, applied after
compression. **Encryption SHALL NOT be described as integrity**, and bespoke cryptography SHALL NOT
be written.

Corruption of a non-essential chunk MAY be recoverable by policy — falling back to a generation or
recovering unaffected scopes — but the engine SHALL NEVER invent authoritative state to fill a gap.

A malformed save SHALL fail diagnostically and SHALL NEVER crash the process.

#### Scenario: Corruption is detected
- **WHEN** a chunk fails its hash
- **THEN** the load SHALL report which chunk failed rather than proceeding

#### Scenario: Nothing is invented
- **WHEN** state cannot be recovered
- **THEN** the load SHALL fail or fall back, and SHALL NOT substitute plausible values

### Requirement: Storage backends and the cloud boundary
Save storage SHALL be behind a **backend interface** — local filesystem, platform storage, cloud
service, server database, and an in-memory backend for tests — while the save model remains the
same.

Cloud transport, quotas, and account association SHALL NOT be owned by this capability. It SHALL
produce artefacts and the metadata a platform service needs.

Conflict resolution between local and remote saves SHALL use **logical metadata** — generation,
campaign identity, simulation point, progress markers, content version — and SHALL NOT be decided by
file modification timestamps.

#### Scenario: A server persists to a database
- **WHEN** a dedicated server persists world state
- **THEN** it SHALL use a backend implementation, with the same save model

#### Scenario: A conflict is decided on meaning
- **WHEN** local and remote saves diverge
- **THEN** resolution SHALL use logical metadata rather than file timestamps

### Requirement: Checkpoints and restore
A **checkpoint** SHALL be a save optimised for rapid in-session restoration, retaining what is
required to restore session, world, and participant state without restarting the application.

Checkpoint restore SHALL increment the simulation epoch, so temporal caches and histories treat
themselves as stale.

Checkpoints MAY be retained in memory as well as on storage, subject to the memory budget.

#### Scenario: Death and restore
- **WHEN** a player dies and the last checkpoint is restored
- **THEN** session, world, and participant state SHALL be restored without an application restart

### Requirement: Save diagnostics and inspection
The engine SHALL provide a **save inspector** showing: manifest contents, scope inventory, counts of
modified, created and tombstoned entities, size by scope, region, component, and plugin, and the
simulation point.

It SHALL answer **why a field is in a save** — naming the component, the field, its persistence
trait, the entity, and when it became dirty — and **why state was not restored**, naming the missing
plugin, failed migration, or unresolved reference.

A **save diff** SHALL compare two saves semantically: entities created and destroyed, fields
changed, and scope-level differences, using stable identifiers.

#### Scenario: Why is this here
- **WHEN** a save is unexpectedly large
- **THEN** the inspector SHALL attribute size by scope, region, component, and plugin

#### Scenario: What changed between saves
- **WHEN** two saves are compared
- **THEN** the difference SHALL be expressed semantically, not as a byte difference

### Requirement: Save performance and testing
Save capture on the main thread SHALL be bounded and budgeted, and the engine SHALL maintain a
**large-world save benchmark**: a world of over a million persistent objects with most regions
unloaded and tens of thousands of dirty records, producing an autosave with no world-wide load, no
full entity scan, and a bounded main-thread cost.

Testing SHALL include: **transactional tests** simulating failure after every write phase and
verifying the previous save remains valid; **fuzzing** with truncated chunks, corrupted hashes,
unknown fields, older schemas, missing plugins, and duplicate identities, all of which SHALL fail
diagnostically rather than crash; **migration tests** across schema chains; and **save-as-fixture**
tests that load a save and run for a period verifying no migration errors and no performance
regression.

Telemetry SHOULD record save failure rate, migration failure rate, and save duration, subject to the
project's privacy policy.

#### Scenario: The world size does not set the save cost
- **WHEN** the large-world benchmark runs
- **THEN** save cost SHALL scale with changes rather than with world size

#### Scenario: Malformed saves never crash
- **WHEN** fuzzed saves are loaded
- **THEN** each SHALL fail with a structured diagnostic

### Requirement: Forbidden save patterns
The following SHALL NOT appear, and each SHALL be checkable:

- Raw runtime memory serialised as a save format
- Runtime entity indices, pointers, or archetype positions used as persistent identity
- Derived caches saved rather than reconstructed
- A save requiring the whole world to be resident
- Save writes performed destructively in place
- One monolithic save blob mixing profile, campaign, and session state
- Runtime migration of cooked content
- Bespoke cryptography, or encryption presented as integrity
- A boolean returned as the result of a failed load
- Inventing authoritative state to replace unrecoverable data

#### Scenario: A proposal is checked
- **WHEN** a change would serialise a component structure by memory copy into a save
- **THEN** it SHALL be flagged against this requirement
