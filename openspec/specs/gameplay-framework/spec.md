# gameplay-framework Specification

## Purpose

Defines **CyberGameplay**: how a game is structured — sessions, rules, participants, teams,
ownership, control, commands, tags, time, spawning and features — without imposing an
object-oriented runtime hierarchy.

The conceptual separations Unreal established are kept: a session that outlives a level, rules
distinct from replicated state, player identity distinct from the thing being driven, possession as
an explicit relationship. What is not kept is the container. Each becomes ECS data or a scoped
service, because a parallel object hierarchy alongside the ECS would undo the reason the ECS exists.

Two decisions carry most of the weight. **One command stream**: human input, artificial
intelligence, network peers, replay playback and automated tests all produce the same semantic
gameplay commands, and the simulation cannot tell them apart — which gives replay, prediction,
testable AI and headless tests as consequences rather than as features. And **control is
many-to-many and channelled**: one player commands two hundred units, two players share a tank, an
AI assists a human's aim, all through bindings rather than through one controller possessing one
pawn.

The performance contract is the third: authoring convenience must never require one gameplay object,
one script object, one virtual update or one controller per simulated entity. Behaviours that can be
batched **compile into generated systems**, and those that cannot are reported at build time rather
than degrading silently at scale.

The specification ends with a **forbidden patterns** requirement naming the architectures it exists
to prevent, because the failure mode here is not a missing feature — it is the gradual reappearance
of the object model the engine was designed to avoid.

## Requirements

### Requirement: Gameplay concepts are data and services
Gameplay structure SHALL be expressed as ECS components, lightweight scoped services, and compiled
definitions. It SHALL NOT introduce a runtime object hierarchy that game objects inherit from.

There SHALL be no mandatory base class for a game entity, and no engine-required derivation chain
of the form actor, pawn, character.

Semantic roles — controllable, character, vehicle, spectator, structure — SHALL be expressed as
components or tags, so that a role is a composition an entity has rather than a type it is.

The editor MAY present composed entities under familiar names; that presentation SHALL NOT imply a
runtime type hierarchy.

#### Scenario: A character is a composition
- **WHEN** a character exists in a world
- **THEN** it SHALL be an entity carrying movement, body, animation, health and control components,
  and SHALL NOT derive from an engine-defined character type

#### Scenario: A role is queryable
- **WHEN** a system needs every controllable character
- **THEN** it SHALL query for those components, with no type test or dynamic cast

### Requirement: Gameplay lifetime model
The engine SHALL define four nested lifetimes, each with a distinct scope:

| Scope | Lifetime | Holds |
|---|---|---|
| **Application** | The process | Engine startup, platform lifecycle, project boot |
| **Game instance** | Launch to shutdown, across sessions | Local users, profiles, online services, saves, progression |
| **Game session** | One logical play session — a match, a campaign run, a replay, an editor play | Participants, teams, rules, session state |
| **World session** | One world's participation in a session | The ECS world and its role |

A game session SHALL be able to span **several worlds in sequence or simultaneously** — lobby, play,
results — without participants, teams, or session state being destroyed and recreated.

Changing world SHALL NOT end the session.

#### Scenario: Players survive a level change
- **WHEN** a session moves from a lobby world to a play world
- **THEN** participants, teams, and session state SHALL persist, and only the world SHALL change

#### Scenario: Several worlds at once
- **WHEN** a session runs a primary world and a preview world simultaneously
- **THEN** each SHALL be a world session with a declared role under the one session

### Requirement: Scoped services
Engine and game services SHALL declare a **scope** — application, game instance, session, world, or
player — determining their lifetime and their visibility.

Services SHALL be reached through an explicit context rather than global lookup. There SHALL NOT be
a set of global gameplay manager singletons.

A service SHALL declare its data access so that the scheduler can order systems that use it, exactly
as for systems. Ad-hoc locking inside gameplay services SHALL NOT be the coordination mechanism.

The game instance SHALL NOT be a container for arbitrary mutable global state; state belongs to a
service with a declared scope.

#### Scenario: Scope determines lifetime
- **WHEN** a session ends
- **THEN** every session-scoped service SHALL be destroyed, and application-scoped services SHALL be
  unaffected

#### Scenario: No global lookup
- **WHEN** a gameplay system needs the team service
- **THEN** it SHALL obtain it from its context, and no global accessor SHALL be required

### Requirement: Gameplay context
Systems and gameplay code SHALL receive a **gameplay context** carrying: the world, the world
session, the game session, the current simulation tick, the services in scope, and the command and
event buffers for the current phase.

The context SHALL be cheap — references and handles, with no allocation — and SHALL be passed
explicitly rather than obtained from a global.

There SHALL be no engine-provided global accessor returning "the world" or "the session" without a
context.

#### Scenario: Everything is reachable from context
- **WHEN** a gameplay system runs
- **THEN** it SHALL reach its world, session, tick, services, and buffers from its context

#### Scenario: No ambient world
- **WHEN** code needs the current world
- **THEN** it SHALL take it from a context, since several worlds may be active

### Requirement: Rules are composable and separate from state
**Game rules** — authoritative logic deciding what is permitted and what happens — SHALL be separate
from **session state** — observable data describing what is true.

Rules SHALL be **composable pieces** rather than a subclass chain: spawn selection, victory
conditions, team formation, respawn, economy, and time rules SHALL be independently selectable.

A **rules asset** SHALL declare a composition with its parameters, so that a game mode is authored
as data. A rule piece MAY reference a native or scripted implementation for logic that data cannot
express.

There SHALL NOT be an engine-provided game mode base class intended for subclassing.

#### Scenario: A mode is a composition
- **WHEN** a skirmish mode is defined
- **THEN** it SHALL be a rules asset composing team, victory, spawn, and economy rules, not a class
  derived from a base mode

#### Scenario: A designer creates a mode
- **WHEN** a designer varies victory conditions and starting resources
- **THEN** they SHALL produce a rules asset without writing engine code

### Requirement: Session state fragments
Session state SHALL be a set of **reflected fragments** registered with the session, not one
monolithic state object.

Each fragment SHALL declare: its authority (server, client, local, or deterministic), its visibility
(everyone, owner, team, authority only, local only), and its persistence class (session transient,
world persistent, profile persistent, save game, or derived).

Those declarations SHALL drive replication, save, replay, and interface observation, so that one
declaration serves all four rather than each being configured separately.

Player state SHALL likewise be fragments — identity, score, progression, economy, statistics —
rather than one player state object.

#### Scenario: One declaration, four consumers
- **WHEN** a match clock fragment declares server authority and full visibility
- **THEN** replication, save, replay, and the interface SHALL each derive their behaviour from that
  declaration

#### Scenario: Fragments are added independently
- **WHEN** a game feature needs new session state
- **THEN** it SHALL register a fragment without modifying an engine state type

### Requirement: Participants, players, and local players
The engine SHALL distinguish:

- a **participant** — anyone taking part: human, bot, remote bot, spectator, or server agent
- a **player** — a participant's session identity, team membership, and player state
- a **local player** — a human at this machine, with an input user, a viewport, an audio listener,
  and local settings

A process SHALL support several local players for split screen and local co-operative play, each
with its own viewport, input user, and listener.

Remote participants SHALL NOT require any local resource — no viewport, no input device, no
listener — and this SHALL be structural rather than a special case.

Participant identity SHALL be stable across control changes, world changes, and reconnection where
the session permits it.

#### Scenario: Split screen
- **WHEN** two players share a machine
- **THEN** each SHALL be a local player with its own viewport, input user, and listener, under one
  process

#### Scenario: Identity survives the avatar
- **WHEN** a player's character is destroyed and they respawn
- **THEN** their participant and player identity, score, and progression SHALL be unchanged

### Requirement: Teams and affiliations
Teams SHALL be first-class, identified by a stable identifier, with membership as data.

Relationships between teams SHALL be **explicit** — self, ally, neutral, hostile — and SHALL NOT be
derived from identifier inequality. Relationships SHALL be changeable at runtime, so alliances,
truces, and betrayals are supported without special cases.

The engine SHALL support **affiliations** beyond teams — faction, squad, party, and project-defined
kinds — so that a unit may belong to a team, a faction, and a squad simultaneously.

Targeting, artificial intelligence, interface, and interest management SHALL consult one
relationship service rather than each implementing its own rule.

#### Scenario: Not everyone else is an enemy
- **WHEN** three teams exist and two are allied
- **THEN** relationships SHALL be read from the relationship matrix, not inferred from team
  identifiers differing

#### Scenario: An alliance forms mid-match
- **WHEN** two teams ally during play
- **THEN** targeting, AI, and interface SHALL immediately observe the change through the same
  service

### Requirement: Ownership, control, and authority are three things
The engine SHALL distinguish, per entity:

| Concept | Meaning |
|---|---|
| **Ownership** | Whose it is — a participant or a team |
| **Control** | Who is driving it right now |
| **Network authority** | Which peer may change its authoritative state |

These SHALL be independently assignable, and no one of them SHALL be inferred from another.

Ownership SHALL be inheritable through entity hierarchies by declaration, so a robot's parts resolve
their owner from the root rather than duplicating it.

#### Scenario: A captured turret
- **WHEN** a player takes control of a turret owned by an enemy faction on a server-authoritative
  session
- **THEN** ownership, control, and authority SHALL each be represented separately and correctly

#### Scenario: Parts inherit ownership
- **WHEN** a composite entity changes owner
- **THEN** its parts SHALL resolve the new owner without each storing a copy

### Requirement: Control sources and bindings
Control SHALL be expressed as a **control source** — human, artificial intelligence, remote peer,
replay, script, or automation — bound to entities through **control bindings**.

A binding SHALL name a **channel** — primary, movement, weapons, camera, turret, command, or a
project-defined channel — so that several sources may control different aspects of one entity.

Control SHALL be **many-to-many**: one source MAY control many entities, and one entity MAY be
controlled by several sources on different channels.

Entity sets SHALL be addressable as **groups**, so a command targets a group rather than copying
thousands of identifiers.

A binding SHALL NOT be limited to one controller possessing one entity.

#### Scenario: A player commands an army
- **WHEN** a player selects two hundred units and issues an order
- **THEN** one binding to a group SHALL carry it, without two hundred control relationships

#### Scenario: Two players share a vehicle
- **WHEN** one player drives and another operates the turret
- **THEN** two sources SHALL bind to the same entity on different channels

#### Scenario: An AI assists a human
- **WHEN** an AI stabilises aim while a human steers
- **THEN** both SHALL hold bindings on the same entity, on different channels

### Requirement: One command stream
Gameplay intent SHALL be expressed as **gameplay commands**: reflected, schema-versioned semantic
operations — move, attack, build, use ability, interact — distinct from raw input events.

Commands SHALL be produced by **all five sources through one path**: human input, artificial
intelligence, remote peers, replay playback, and automated tests. The simulation SHALL NOT be able
to distinguish their origin.

Raw input SHALL NOT reach gameplay systems. Input actions (see `core-platform-abstraction`) produce
commands; gameplay consumes commands.

A command SHALL declare: its reliability, whether it may be predicted, whether it is local-only or
authoritative, and its validation requirements.

Command submission SHALL scale: submission SHALL NOT serialise through a single lock, and commands
SHALL be accumulated per worker and committed deterministically.

#### Scenario: Origin is indistinguishable
- **WHEN** a move command arrives from a human, an AI, a network peer, and a replay
- **THEN** the simulation SHALL process all four identically

#### Scenario: Replay is intent, not state
- **WHEN** a session is replayed
- **THEN** it SHALL be reconstructed from the recorded command stream and the session seed

#### Scenario: A test drives a game
- **WHEN** an automated test issues commands
- **THEN** it SHALL use the same path as a player, with no test-specific gameplay entry point

### Requirement: Command validation returns reasons
Command validation SHALL return a **structured result**: whether the command is permitted, and when
it is not, tagged reasons with the data behind them.

`true` or `false` SHALL NOT be the validation interface.

One validation implementation SHALL serve: the interface explaining why an action is unavailable,
artificial intelligence deciding what to attempt, the authority rejecting an illegal command, and
tests asserting behaviour — so that the four cannot disagree.

Validation SHALL be callable **without executing** the command, so the interface can grey out an
action and state why before the player acts.

The engine SHALL validate structurally before game-specific logic runs: that the participant exists,
that it controls the target on the required channel, and that the target accepts the command's
capability.

#### Scenario: The interface explains itself
- **WHEN** a build action is unavailable
- **THEN** the interface SHALL show the tagged reason and its data, from the same validation the
  server would apply

#### Scenario: Structural checks come first
- **WHEN** a participant issues a command for an entity it does not control
- **THEN** it SHALL be rejected by the engine before game logic runs

### Requirement: Capabilities
Entities SHALL declare the **capabilities** they accept — movable, attack capable, build capable,
interactable, ability user, and project-defined kinds — as components or a compact mask.

Command routing SHALL use capabilities to filter targets, so a command addressed to a group applies
only to the members that can accept it, and the remainder are reported rather than silently ignored.

#### Scenario: A mixed selection
- **WHEN** a build command is issued to a group containing builders and soldiers
- **THEN** only the builders SHALL receive it, and the exclusion SHALL be reportable

### Requirement: Gameplay tags
The engine SHALL provide **hierarchical gameplay tags**: authored as dotted text, declared in a
registry, cooked to identifiers, and compared as integers.

Strings SHALL NOT be the runtime identity of a tag. Hierarchical queries — matching
`Unit.Robot` against `Unit.Robot.Harvester` — SHALL resolve through compact metadata, not string
prefix comparison.

Tags SHALL be usable for classification, state, damage types, surfaces, abilities, phases, and
validation reasons, and SHALL be available to every subsystem.

**Gameplay tags SHALL NOT replace ECS tag components.** ECS tags are structural and make queries
cheap by changing an archetype; gameplay tags are dynamic state carried in a set. Using a gameplay
tag where an archetype query belongs turns a query into a scan, and tooling SHALL be able to report
where this occurs.

#### Scenario: Integer comparison
- **WHEN** a tag is tested at runtime
- **THEN** the test SHALL be an integer or set operation, never a string comparison

#### Scenario: Hierarchy matches
- **WHEN** a query for `Unit.Robot` is evaluated
- **THEN** entities tagged `Unit.Robot.Harvester` SHALL match

#### Scenario: The distinction is enforced
- **WHEN** a frequently queried classification is expressed as a gameplay tag rather than an ECS tag
- **THEN** tooling SHALL be able to report it as a performance issue

### Requirement: Events and messages
Gameplay events SHALL be **typed** and delivered through the ECS event mechanism, accumulated in
per-worker buffers and committed deterministically (see `core-jobs-and-concurrency`).

Delivery mode SHALL be declared: immediate local, end of phase, next tick, or networked. **Phase
buffered SHALL be the default**; arbitrary synchronous cascades through gameplay code SHALL be
discouraged and SHALL NOT be the default.

Events SHALL be targetable: global, to an entity, to a participant, to a team, or to a world region.

A **message channel** identified by a gameplay tag SHALL be provided for loose coupling, so the
interface can observe an occurrence without knowing which system produced it.

An event SHALL declare whether it is replay relevant, network relevant, or telemetry relevant; raw
event streams SHALL NOT automatically become network or replay streams.

**Commands and events SHALL NOT be conflated**: a command asks for something to happen, an event
reports that something happened.

#### Scenario: No synchronous cascade
- **WHEN** damage is applied
- **THEN** the resulting event SHALL be delivered at the declared point, not by a chain of
  synchronous calls through unrelated systems

#### Scenario: The interface observes without coupling
- **WHEN** an objective completes
- **THEN** the interface SHALL observe a tagged message without referencing the system that
  produced it

### Requirement: Game phases
A session SHALL have a **current phase**, expressed as a gameplay tag so phases are hierarchical and
extensible without changing an engine enumeration.

Rules SHALL validate phase transitions, and transitions SHALL emit entering and leaving events
recorded with the authoritative simulation tick.

Systems and features SHALL be able to declare the phases during which they are active, so that a
phase change activates and deactivates work rather than every system testing the phase itself.

#### Scenario: Phases are extensible
- **WHEN** a game adds an overtime phase
- **THEN** it SHALL add a tag and transition rule, without modifying an engine type

#### Scenario: Transitions are recorded
- **WHEN** a match begins
- **THEN** the transition SHALL be recorded with its tick, so replay and network peers agree on when
  it happened

### Requirement: Spawning
Spawning SHALL be a service taking a **spawn request** — an entity template, an owner, a team, and a
context — and returning a result, applying declared **spawn rules** to select a location.

Spawn policies SHALL include at minimum: exact position, spawn point, region, nearest safe,
weighted random, formation, navigation-reachable, and authority-assigned.

**Batch spawning SHALL be first-class**: spawning many instances of one template SHALL be one
operation using the entity template's archetype blocks, not repeated single spawns.

Spawning SHALL support **reservation**: a location may be reserved while dependencies stream, so two
simultaneous requests do not select the same point.

Spawn points SHALL be representable as spatial metadata; instantiating an entity per spawn point
SHALL NOT be required.

#### Scenario: An army arrives at once
- **WHEN** ten thousand units are spawned in a formation
- **THEN** they SHALL be created by batch instantiation, not by ten thousand separate spawns

#### Scenario: Two players do not collide
- **WHEN** two respawn requests select simultaneously
- **THEN** reservation SHALL prevent both taking the same point

### Requirement: Time domains
The engine SHALL provide **time domains** — real, gameplay, simulation, interface, cinematic, and
project-defined — each with its own elapsed time, scale, and paused state.

There SHALL NOT be one global delta time that all systems consume.

Pausing SHALL be a per-domain policy: gameplay may pause while the interface animates, audio
continues, and networking proceeds. A session MAY forbid pausing entirely.

Systems SHALL declare the domain they advance with.

#### Scenario: Menus animate while paused
- **WHEN** gameplay is paused
- **THEN** the interface domain SHALL continue and gameplay systems SHALL not advance

#### Scenario: Slow motion is scoped
- **WHEN** gameplay time is scaled to one half
- **THEN** the interface SHALL remain at full speed

### Requirement: The simulation clock
Gameplay simulation SHALL advance in **fixed simulation ticks** with a monotonically increasing tick
number, and systems MAY run at reduced rates relative to it — strategic reasoning at a few hertz,
distant agents lower still.

The clock itself, the exact rational tick rate, catch-up bounds, simulation epochs, and the commit
boundary at which a tick becomes authoritative are defined in `simulation-and-determinism`. This
capability consumes them.

Scheduled gameplay SHALL be expressed in **ticks**, not wall-clock time: "at tick 8842" is
reproducible; "in 3.0 seconds" is not.

A moment SHALL be identified by **epoch and tick**, since rollback moves the tick backwards.

Timers SHALL be provided per time domain, implemented so that many timers cost bounded work — a
bucketed or wheel structure rather than one heap entry and one callback per timer per tick.

Network and replay systems SHALL use the same tick numbering, so peers and recordings refer to the
same instants.

#### Scenario: Deterministic scheduling
- **WHEN** a delayed effect is scheduled
- **THEN** it SHALL fire at a specific tick, identically on every peer and in replay

#### Scenario: Many timers are cheap
- **WHEN** fifty thousand timers are pending
- **THEN** advancing a tick SHALL cost work proportional to the timers actually due

#### Scenario: The same tick is not the same moment
- **WHEN** a rollback returns to a tick already simulated
- **THEN** the two occurrences SHALL be distinguishable by epoch

### Requirement: Deterministic random streams
Randomness used by gameplay SHALL come from **named streams** derived from the session seed, not
from a global generator.

Stream derivation, counter-based sampling, and inspection are defined in
`simulation-and-determinism`; this capability requires their use.

Streams SHALL be independent, so that consuming randomness in one system does not perturb another's
sequence — which is what makes a change in one feature alter unrelated outcomes in replay.

Stream state SHALL be part of session state where reproducibility requires it, and streams used for
presentation only SHALL be declared as such.

#### Scenario: Independent sequences
- **WHEN** a combat stream and a loot stream are both consumed
- **THEN** changing how much randomness combat uses SHALL NOT change loot outcomes

#### Scenario: Reproducible session
- **WHEN** a session is replayed from its seed and command stream
- **THEN** random outcomes SHALL match the original

#### Scenario: Sampling is parallel-safe
- **WHEN** many entities sample randomness concurrently
- **THEN** each SHALL derive its value from stable inputs, with no shared generator and no ordering
  dependency

### Requirement: Interaction
The engine SHALL provide an **interaction framework**: an interactor queries what is available, an
interactable provides **options**, and selecting one produces a gameplay command.

An option SHALL carry: its action tag, display text, range, conditions, and the command it produces
— so the interface can present it and artificial intelligence can evaluate it without either
knowing the implementation.

Interaction queries SHALL be **batchable** and SHALL use spatial acceleration; per-entity ray casts
from thousands of agents SHALL NOT be the mechanism.

Precision SHALL be selectable: a focused human interaction may be precise while bulk agent queries
use coarser spatial tests.

#### Scenario: The interface and the AI agree
- **WHEN** a resource node offers mine, repair, and inspect
- **THEN** the interface and an AI agent SHALL both read the same options, and selecting one SHALL
  produce a command

#### Scenario: Bulk interaction is cheap
- **WHEN** thousands of agents evaluate nearby interactions
- **THEN** queries SHALL be batched against spatial structures

### Requirement: Gameplay features
Gameplay functionality SHALL be packageable as **features**: named units declaring the components,
systems, rules, tags, input contexts, assets, interface, and world layers they contribute.

Features SHALL be activatable and deactivatable per session and per world, with states installed,
loaded, registered, activated, and deactivated.

Game modes SHALL be expressible as **compositions of features and rules** rather than as subclasses.

Feature activation is **content-level**, distinct from module and plugin loading (see
`project-and-plugins`), though a feature MAY require a plugin.

Features SHALL declare dependencies on other features, resolved before activation.

#### Scenario: A mode is a set of features
- **WHEN** a strategy skirmish mode is defined
- **THEN** it SHALL compose base strategy, economy, fog of war, and a victory rule, rather than
  deriving from a mode class

#### Scenario: Content adds a feature
- **WHEN** downloadable content introduces a game mode
- **THEN** it SHALL ship as a feature activated for sessions that select it

### Requirement: Gameplay lifecycle and death
Gameplay lifecycle SHALL be distinct from entity lifetime.

**Gameplay death SHALL NOT imply entity destruction.** An entity reaching a defeated state may
animate, leave a wreck, be revivable, or persist indefinitely; destruction is a separate decision.

Removal SHALL carry a structured **despawn reason** — destroyed, streamed out, session ended,
replaced, scripted, or editor — extensible by gameplay tag, so systems can distinguish a unit dying
from a region unloading.

Structural changes — spawn, destroy, add or remove component — SHALL be recorded in command buffers
and applied at defined points, never mutated during parallel iteration.

#### Scenario: A wreck remains
- **WHEN** a vehicle is destroyed in gameplay
- **THEN** it MAY remain as a wreck entity, and gameplay death SHALL NOT have destroyed it

#### Scenario: Streaming is not death
- **WHEN** a cell unloads and its entities are removed
- **THEN** the despawn reason SHALL be streaming, and death-triggered logic SHALL NOT run

### Requirement: Gameplay references
Long-lived gameplay references SHALL use **persistent identity** (see
`world-partition-and-streaming`), not runtime entity identifiers, so that a reference survives
streaming, saving, and world changes.

Resolution SHALL return a defined state — resolved, not resident, destroyed, or unknown — and SHALL
NOT block or fault.

Runtime entity identifiers SHALL be used within a tick and SHALL NOT be persisted or held across
world transitions.

#### Scenario: A quest target is unloaded
- **WHEN** an objective refers to an entity in an unloaded region
- **THEN** the reference SHALL remain valid and resolve to not resident

#### Scenario: References survive a save
- **WHEN** a session is saved and reloaded
- **THEN** gameplay references SHALL resolve to the same objects

### Requirement: Gameplay indexes
The engine SHALL maintain derived **indexes** for the queries gameplay makes constantly: entities by
owner, by team, by affiliation, and by gameplay tag.

These SHALL be maintained incrementally from ECS state and SHALL be caches, not authoritative
storage: rebuilding them from the world SHALL produce the same result.

Answering "what does this participant own" SHALL NOT require scanning every entity.

#### Scenario: Ownership query is indexed
- **WHEN** a player's units are enumerated in a hundred-thousand-entity world
- **THEN** the query SHALL use an index rather than a full scan

#### Scenario: Indexes are derived
- **WHEN** an index is discarded and rebuilt
- **THEN** the result SHALL be identical to the incrementally maintained one

### Requirement: Network integration
Gameplay SHALL be **network-first but not network-dependent**: the same code SHALL run in a
single-player session with no networking active.

Client-to-server gameplay intent SHALL travel as **gameplay commands**, which are the channel that
carries prediction, authority validation, and replay recording.

The engine SHALL validate on receipt that the participant exists, controls the target on the
required channel, and is permitted the command, **before** game-specific logic runs.

Session state fragments and player state fragments SHALL replicate according to their declared
authority and visibility.

Commands declared local-only SHALL NOT be transmitted.

#### Scenario: Single-player uses the same code
- **WHEN** a session runs with no networking
- **THEN** commands, validation, and state SHALL follow the same path, with transmission absent

#### Scenario: An illegal command is rejected structurally
- **WHEN** a client issues a command for an entity it does not control
- **THEN** the server SHALL reject it before game logic runs, and the rejection SHALL be reportable

### Requirement: Save and replay contracts
Gameplay state SHALL declare a persistence class — session transient, world persistent, profile
persistent, save game, or derived — and those declarations SHALL determine what a save captures.

A **replay** SHALL be reconstructible from: the session's initial state, its seed, its stream of
gameplay commands, recorded external results where determinism does not suffice, and periodic
checkpoints for seeking.

Replay playback SHALL be a control source producing commands, so playback exercises the same
simulation path as live play.

The mechanisms — the command log, external result records, snapshot kinds, checkpoints, seeking,
rollback, and the side-effect ledger — are defined in `replay-and-rollback`, and save encoding,
scopes, and migration in `save-and-persistence`. This capability declares what gameplay contributes
to them.

Command and gameplay state schemas SHALL be versioned, so a replay or save from an older build is
either migrated where supported or rejected clearly.

#### Scenario: Replay drives the game
- **WHEN** a replay is played
- **THEN** it SHALL be a control source issuing recorded commands, not a separate playback path

#### Scenario: Version mismatch is explicit
- **WHEN** a replay's command schema differs from the build's
- **THEN** it SHALL be migrated where supported or rejected with a diagnostic, never misinterpreted

#### Scenario: Non-reproducible results are recorded
- **WHEN** an authoritative outcome comes from a service or an inference
- **THEN** it SHALL be recorded as an external result and consumed from the record during replay

### Requirement: Headless operation
The gameplay framework SHALL be **fully functional with no renderer, no audio, no interface, and no
GPU**.

This SHALL be a requirement rather than a build configuration: a gameplay system that requires a
camera, viewport, material, or audio device SHALL be a defect.

Continuous integration SHALL run gameplay tests headless, and the dedicated server SHALL link no
rendering code.

#### Scenario: The server needs no renderer
- **WHEN** a dedicated server runs a session
- **THEN** no rendering, audio, or interface code SHALL be required or linked

#### Scenario: Tests run without a display
- **WHEN** gameplay tests run in continuous integration
- **THEN** they SHALL execute with no display, GPU, or audio device

### Requirement: Behaviour ergonomics compile to systems
The engine SHALL offer object-like gameplay ergonomics — attaching a behaviour with lifecycle
callbacks — in both C++ and Swift.

Behaviours whose callbacks read and write **declared component data** SHALL be compiled into
**generated systems** iterating chunks, so that many instances of one behaviour cost one system
rather than one call per instance.

Behaviours that cannot be batched — those calling arbitrary script per entity per tick, holding
unbounded per-instance state, or reaching outside declared access — SHALL remain per-instance, and
the compiler SHALL **report which behaviours batched, which did not, and why**, so the cost is known
at build time.

Authoring convenience SHALL NOT require one gameplay object, one script object, one virtual update
call, or one controller per simulated entity.

#### Scenario: A hundred thousand trees cost one system
- **WHEN** a hundred thousand entities carry one batchable behaviour
- **THEN** it SHALL execute as one system over chunks, not as a hundred thousand callbacks

#### Scenario: The limit is reported, not hidden
- **WHEN** a behaviour cannot be batched
- **THEN** the build SHALL report it with the reason, rather than silently degrading at scale

### Requirement: Performance contracts
The gameplay framework SHALL meet these architectural targets on a high-end desktop target, and they
SHALL be benchmarked rather than asserted:

| Property | Target |
|---|---|
| Active gameplay entities | 100 000 without the framework itself dominating frame time |
| Resident simple entities | Architecturally supported at 1 000 000 |
| Batch spawn | 10 000 entities without 10 000 individual allocations |
| Command submission | 100 000 commands per second without a central lock |
| Ownership, team, and tag queries | Indexed, not scans |
| Tag tests | Integer or set operations |
| Primary system loops | No virtual dispatch per entity |

The framework's own cost SHALL be a reported fraction of simulation frame time, so that regressions
are attributable to it rather than to the game.

#### Scenario: The framework is not the bottleneck
- **WHEN** the strategy stress scenario runs
- **THEN** gameplay framework overhead SHALL be a small, reported fraction of simulation time

#### Scenario: Targets are measured
- **WHEN** a change affects framework overhead
- **THEN** the benchmarks SHALL detect it rather than the regression being noticed in a game

### Requirement: Forbidden patterns
The following SHALL NOT appear in the engine's gameplay framework, and each SHALL be checkable in
review or by tooling:

- One heap-allocated gameplay object per ECS entity, created to provide framework semantics
- A common actor or pawn base class that gameplay entities must derive from
- Scene hierarchy traversal as the gameplay update mechanism
- A virtual per-object tick as the primary simulation model
- Conflation of participant, player, controlled entity, owner, team, or network authority
- Conflation of raw input events with gameplay commands
- Conflation of commands with events
- Strings as runtime gameplay tag identity
- Global gameplay manager singletons as the default subsystem model
- The game instance used as a store for arbitrary mutable global state
- Rendering, audio, or interface dependencies required by gameplay
- Representing a large controlled group as one control relationship per entity

A proposal introducing any of these SHALL be evaluated against this requirement.

#### Scenario: A proposal is checked against the list
- **WHEN** a change would add a base class every gameplay entity derives from
- **THEN** it SHALL be flagged against this requirement and require an explicit decision to proceed

#### Scenario: Scale reveals a violation
- **WHEN** a system allocates one object per entity to provide framework semantics
- **THEN** it SHALL be treated as a defect rather than an implementation detail

### Requirement: Gameplay diagnostics
The engine SHALL provide gameplay inspection covering: the session and its phase, participants,
players, teams and their relationships, ownership, control bindings and channels, capabilities,
gameplay tags, active features, time domains, and pending timers.

Selecting an entity SHALL report its owner, its controllers and their channels, its team and
affiliations, its capabilities, and its tags.

A **command timeline** SHALL show commands by tick and source, and a **rule debugger** SHALL show
rejected commands with their structured reasons and the data behind them.

#### Scenario: Why was this rejected
- **WHEN** a build command is rejected
- **THEN** the debugger SHALL show the tagged reason and the values that caused it

#### Scenario: Who is driving this
- **WHEN** an entity is inspected
- **THEN** its owner, controllers, channels, team, and tags SHALL be shown distinctly

### Requirement: Scope and extension
This capability SHALL cover universal game structure only. Abilities, attributes, inventories,
quests, and objectives SHALL be **optional modules built on it**, not parts of it, so that a game
does not pay for systems it does not use.

Game-specific concepts — resources, weapons, crafting, mana — SHALL remain game code.

The engine MAY provide optional gameplay modules; they SHALL be separately activatable features.

Gameplay visual scripting is deferred. Commands, events, tags, rules, and validation are reflected
schemas so that a future gameplay graph can **compile to a system** rather than be interpreted per
entity, and that seam SHALL be preserved.

#### Scenario: A game without abilities pays nothing
- **WHEN** a project does not use the ability module
- **THEN** no ability code or data SHALL be present in its build

#### Scenario: The scripting seam is preserved
- **WHEN** a proposal would make commands or validation unable to be expressed as compiled data
- **THEN** it SHALL be flagged against this requirement
