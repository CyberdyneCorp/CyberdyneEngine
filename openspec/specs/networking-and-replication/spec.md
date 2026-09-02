# networking-and-replication Specification

## Purpose

Defines **CyberNet**: multiplayer as an ECS-native, server-authoritative framework built for
simulation-heavy games.

The central decision is to **replicate component state, not objects**. Replication operates over
packed component arrays through compiled schemas, so a thousand transforms serialise as a loop over
contiguous data rather than a traversal of a thousand independent network objects.

Three **network modes** are supported because they are genuinely different architectures, not
quality settings: `SnapshotAuthoritative` for most games, `Rollback` for latency-sensitive action,
and `Lockstep` for strategy and simulation, where replicating commands instead of state makes
bandwidth independent of unit count. Each declares its prerequisites, and the engine verifies them
rather than letting a mismatch appear later as a desync.

**Interest management is a scheduler, not a predicate.** Relevance produces candidates; a priority
scheduler then decides what is sent this tick, at what frequency, and at what precision, within a
bandwidth budget — the same pattern used for VFX, audio, AI, and animation. Distant entities are
degraded before they are dropped, and bounded staleness prevents starvation.

Two limitations are stated rather than implied. **Cross-platform lockstep is not supported**,
because the physics backend does not guarantee cross-platform determinism and the engine has no
fixed-point path. Replication cells derive from the world partition
(see `world-partition-and-streaming`) rather than from a second spatial subdivision owned by
networking, so client streaming and server relevance refer to the same content by the same
identifiers.

## Requirements

### Requirement: Network modes
A world SHALL declare one of three **network modes**, each with distinct requirements and
guarantees:

| Mode | Replicates | Simulation contract |
|---|---|---|
| `SnapshotAuthoritative` | Component state from the authority to interested peers | Server authoritative; clients may predict locally |
| `Rollback` | Inputs plus authoritative state, with client-side snapshot, rollback and replay | Client simulation must be deterministic and snapshot-restorable |
| `Lockstep` | Commands only; every peer simulates the full world | All participating peers must produce bit-identical simulation |

The mode SHALL be a project or world-level declaration, and the engine SHALL verify at startup and
at session start that the declared mode's prerequisites hold, failing with a clear diagnostic
rather than desyncing later.

Modes SHALL share the transport, replication schemas, and interest management; only the simulation
contract differs.

#### Scenario: RTS chooses lockstep
- **WHEN** a strategy game with 20,000 units selects `Lockstep`
- **THEN** it SHALL replicate player commands rather than unit state, and bandwidth SHALL be
  independent of unit count

#### Scenario: Prerequisites are checked, not assumed
- **WHEN** a world declares `Lockstep` while a subsystem in use is declared non-deterministic
- **THEN** startup SHALL fail naming the subsystem, rather than the mismatch appearing as a desync

#### Scenario: Different worlds, different modes
- **WHEN** a project has a lockstep skirmish mode and a snapshot-replicated campaign
- **THEN** each world SHALL declare its own mode

### Requirement: Lockstep requirements and limits
`Lockstep` mode SHALL require: a fixed simulation timestep, deterministic physics, deterministic
AI scheduling, deterministic animation root motion, seeded random state that is part of simulation
state, deterministic ECS system ordering, and exclusion of every subsystem declared
non-deterministic (VFX, non-pinned ML inference, adaptive budget controllers).

**Cross-platform lockstep SHALL NOT be supported.** The physics backend does not guarantee
cross-platform determinism, and the engine provides no fixed-point or soft-float simulation path.

A lockstep session SHALL therefore declare its **compatibility scope** — platform, architecture,
and binary build identifier — and the engine SHALL verify that every participant matches before
the session starts.

Lockstep sessions SHALL exchange periodic **state hashes** so divergence is detected within a
bounded number of ticks, reported with the diverging tick and, where determinable, the diverging
system.

#### Scenario: Mismatched participants are rejected
- **WHEN** a peer attempts to join a lockstep session with a different build or platform
- **THEN** it SHALL be rejected at join time with the mismatch named

#### Scenario: Divergence is caught quickly
- **WHEN** two peers diverge
- **THEN** the periodic hash comparison SHALL detect it within the configured interval and report
  the tick, rather than the match drifting silently

#### Scenario: The limitation is documented, not implied
- **WHEN** a team plans a cross-platform lockstep title
- **THEN** the documentation SHALL state plainly that this is unsupported and what it would
  require, rather than leaving it to be discovered

### Requirement: Authority model
Every replicated entity SHALL have an **authority**: the peer permitted to change its
authoritative state.

Topologies SHALL be supported by configuration:
- **Dedicated server** — the server has authority over all gameplay entities
- **Listen server** — one peer is both host and player
- **Peer-to-peer with distributed authority** — each peer owns its entities

Authority SHALL be transferable at runtime, with a defined handover protocol.

#### Scenario: Client cannot change server-owned state
- **WHEN** a client attempts to write a component on a server-authoritative entity
- **THEN** the change SHALL be local-only and overwritten by the next authoritative update, and
  in development builds a diagnostic SHALL warn

#### Scenario: Authority handover
- **WHEN** authority over an entity transfers from one peer to another
- **THEN** the protocol SHALL ensure exactly one peer considers itself authoritative at any time,
  with the transition acknowledged

### Requirement: Authority migration seams
Network identity and ownership SHALL be designed so that **authority migration** between server
processes remains possible without reworking them.

Network entity ids SHALL be globally unique across a session rather than server-local. Authority
SHALL be an explicit property of an entity, transferable through a defined handover protocol that
guarantees exactly one authority at any time.

Distributed simulation across multiple server processes SHALL NOT be implemented, and this SHALL be
recorded as deferred rather than assumed impossible.

#### Scenario: Handover is unambiguous
- **WHEN** authority over an entity transfers
- **THEN** the protocol SHALL ensure exactly one peer considers itself authoritative throughout,
  with the transition acknowledged

#### Scenario: A change would close the seam
- **WHEN** a proposal makes network ids server-local or authority implicit
- **THEN** it SHALL be flagged against this requirement and either revised or accepted as an
  explicit decision to abandon distributed simulation

### Requirement: Transport abstraction
`Transport` SHALL be the engine-defined interface for moving bytes between peers, providing:
connect, disconnect, send on a numbered channel with a delivery mode, receive, and connection
state and statistics (round-trip time, packet loss, bandwidth).

Delivery modes: `Unreliable`, `UnreliableSequenced`, `ReliableUnordered`, `ReliableOrdered`.

The engine SHALL ship: a **UDP transport** with its own reliability layer, a **WebSocket
transport** for browser targets, and a **local transport** for single-process testing. Additional
transports — QUIC, platform networking services, and console services — SHALL be implementable as
modules behind the same interface.

Transport implementations SHALL provide, or be composed with, **encryption and authentication**,
replay protection, and sequence validation; no transport SHALL be offered as a default without
them.

No transport library type SHALL appear outside its backend module.

#### Scenario: Channel independence
- **WHEN** a reliable chat message is delayed by retransmission
- **THEN** unreliable state updates on another channel SHALL not be head-of-line blocked

#### Scenario: Transport swapped for tests
- **WHEN** an integration test runs client and server in one process
- **THEN** the local transport SHALL be used with configurable simulated latency and loss, and no
  gameplay code SHALL change

#### Scenario: Platform transport
- **WHEN** a title ships on a platform with its own networking service
- **THEN** that service SHALL be implementable as a transport backend with no change above the
  interface

#### Scenario: Secure by default
- **WHEN** a transport is used for a shipping build
- **THEN** encryption, authentication, and replay protection SHALL be in effect

### Requirement: Network simulation
The transport layer SHALL provide a **network condition simulator** applying configurable latency,
jitter, packet loss, duplication, and reordering, usable in development builds and tests.

#### Scenario: Testing under adverse conditions
- **WHEN** a developer enables 150 ms latency with 5 % loss
- **THEN** the game SHALL run against those conditions locally, exercising prediction and
  reconciliation

### Requirement: Replication schemas
Replicated components SHALL be described by **replication schemas**: declarations of which fields
cross the wire and how each is encoded.

A schema SHALL specify per field: the encoder (raw, quantised scalar, quantised vector, compressed
quaternion, dictionary index, bitfield), its parameters (range and bit count, or precision), the
send condition, the target filter, and a priority contribution.

Schemas SHALL be **compiled** to serialisation and deserialisation code, not interpreted per field
per entity, so that replicating a thousand instances of a component is a loop over a packed array
with a known encoder.

Schemas SHALL be validated against the reflected component type at cook time; a field that no
longer exists, or a range that cannot represent the field's declared bounds, SHALL be a cook error.

Schema identity SHALL be versioned and verified at connection time, so peers with mismatched
schemas are rejected rather than misinterpreting each other's data.

#### Scenario: Packed serialisation
- **WHEN** 1,000 entities' transforms are replicated
- **THEN** they SHALL be serialised by iterating a packed component array through compiled encoder
  code, not by per-field reflection per entity

#### Scenario: Schema drift is caught
- **WHEN** a component field is renamed without updating its schema
- **THEN** cooking SHALL fail naming the field

#### Scenario: Mismatched peers are rejected
- **WHEN** a client with an older schema set connects
- **THEN** the mismatch SHALL be detected at connection and reported, rather than producing
  corrupt state

### Requirement: Component replication
Replication SHALL be declarative: components and fields marked `Replicated` are synchronised from
the authority to interested peers, described by a **replication schema** specifying encoding.

Per-field configuration SHALL include: a condition (always, on change, or a predicate), an encoder
with its quantisation and precision, a priority contribution, and a target filter (everyone, the
owner only, or everyone except the owner).

Replication SHALL use ECS change detection so unchanged data is not sent, and SHALL **delta
encode** against the last state each peer acknowledged.

Replication SHALL operate over packed component arrays, so serialising many instances of a
component is a loop over contiguous data rather than a traversal of independent objects.

#### Scenario: Only changes are sent
- **WHEN** an entity's health is unchanged
- **THEN** no health data SHALL be transmitted for it

#### Scenario: Quantised transform
- **WHEN** a position is declared with 1 cm precision within a bounded range
- **THEN** it SHALL be quantised to the minimum bits needed, not sent as three 32-bit floats

#### Scenario: Owner-only field
- **WHEN** a field is marked owner-only
- **THEN** it SHALL be sent only to the peer that owns the entity, so other clients never receive
  it

#### Scenario: Priority under bandwidth pressure
- **WHEN** the outgoing bandwidth budget is exceeded
- **THEN** lower-priority updates SHALL be deferred to later packets, with the deferral bounded so
  they are not starved indefinitely

#### Scenario: Homogeneous batch
- **WHEN** many entities share a replicated component
- **THEN** they SHALL be serialised as a batch over the packed array rather than per entity

### Requirement: Snapshots, baselines and delta encoding
Replication SHALL be **snapshot-based with delta encoding**: each peer has an acknowledged
**baseline**, and subsequent updates encode differences against it.

The system SHALL maintain per peer: the last acknowledged snapshot, a bounded history of
unacknowledged snapshots, and per-entity change masks indicating which fields differ.

When a peer's acknowledgement falls outside the retained history, the server SHALL send a fresh
baseline rather than an undecodable delta.

Snapshots SHALL be self-consistent: a delta SHALL never mix state from different simulation ticks
for the same entity.

#### Scenario: Delta against acknowledged state
- **WHEN** a client has acknowledged snapshot 100 and the server sends snapshot 106
- **THEN** the payload SHALL encode differences from 100, with a change mask indicating which
  fields are present

#### Scenario: History exhausted
- **WHEN** a client's acknowledgement is older than the retained snapshot history
- **THEN** the server SHALL send a full baseline and resume delta encoding from it

#### Scenario: Tick consistency
- **WHEN** an entity's fields are split across packets
- **THEN** all of them SHALL derive from the same simulation tick, so the client never observes a
  torn state

### Requirement: Wire compression
The replication layer SHALL provide a documented compression toolkit applied through schemas:

- **Bit packing** — fields packed to their declared bit widths without byte alignment
- **Quantisation** — scalars and vectors mapped to integer ranges with declared precision
- **Quaternion compression** — smallest-three or equivalent, with declared bit budget
- **Change masks** — a bitfield indicating which fields are present, rather than sending
  identifiers
- **Dictionary and string tables** — repeated identifiers and strings sent once and referenced
- **Run-length encoding** — for repeated values across an entity range
- **Delta against baseline** — the default for all state

The achieved bits per entity per component SHALL be measurable, and the schema editor SHALL report
the theoretical and observed size.

#### Scenario: Position is not three floats
- **WHEN** a position is declared with 1 cm precision within a 2 km cell
- **THEN** it SHALL be encoded in the minimum bits that range requires, not 96 bits

#### Scenario: Cost is visible while authoring
- **WHEN** a developer adds a field to a replicated component
- **THEN** the schema tooling SHALL report the added bits per entity per update

### Requirement: Entity spawning and lifetime
Entity creation and destruction SHALL be replicated: the authority spawns from a **networked
prefab** identified by a stable id, and interested peers instantiate it.

Each replicated entity SHALL have a **network id** stable across peers, distinct from the local
`Entity` id.

Late-joining peers SHALL receive a **baseline** of all relevant entities before incremental
updates begin.

#### Scenario: Late join
- **WHEN** a peer connects mid-session
- **THEN** it SHALL receive a baseline snapshot of all entities in its interest set, then deltas

#### Scenario: Spawn ordering
- **WHEN** an entity references another that has not yet been spawned on a peer
- **THEN** the reference SHALL resolve to null and re-resolve when the target arrives, rather
  than failing

### Requirement: Remote procedure calls
RPCs SHALL be declared with: a direction (`ToServer`, `ToClients`, `ToOwner`, `ToTarget`), a
delivery mode, and an authority requirement.

RPC parameters SHALL be typed and serialized with the same machinery as replicated fields.

The engine SHALL validate on receipt: that the sender is permitted to invoke this RPC on this
entity, and that parameters are within declared bounds.

#### Scenario: Unauthorised RPC
- **WHEN** a client invokes a server-only RPC it is not permitted to call
- **THEN** the server SHALL reject it, log it, and optionally apply a rate-limit or disconnect
  policy

#### Scenario: RPC ordering with state
- **WHEN** an RPC depends on state replicated in the same tick
- **THEN** the ordering guarantee SHALL be documented, and the RPC SHALL be delivered after that
  tick's state is applied

### Requirement: Interest management
The server SHALL determine, per peer, which entities are **relevant**, producing the candidate set
that the priority scheduler then selects from.

Relevance SHALL be computable by: replication cell membership, distance from the peer's viewpoint,
team or faction membership, ownership, explicit always-relevant marking, and custom predicates.

Entities entering relevance SHALL receive a baseline; entities leaving SHALL be explicitly
dropped so the client can clean up.

Relevance SHALL be evaluated incrementally where possible, using cell membership changes rather
than re-evaluating every entity against every peer each tick.

#### Scenario: Large world
- **WHEN** a world contains 10 000 entities and a peer can perceive 200
- **THEN** only those 200 SHALL be replicated to that peer

#### Scenario: Relevance change
- **WHEN** an entity leaves a peer's relevance set
- **THEN** the peer SHALL be told to remove it, rather than being left with a frozen ghost

#### Scenario: Information leakage
- **WHEN** an entity is not relevant to a peer
- **THEN** no data about it SHALL be sent, so a modified client cannot observe it

#### Scenario: Incremental evaluation
- **WHEN** a peer moves slightly within a cell
- **THEN** relevance SHALL not be recomputed for every entity in the world

### Requirement: Priority scheduling and network level of detail
Interest management SHALL produce **candidates**; a **priority scheduler** SHALL then decide which
candidates are sent this tick, at what frequency, and at what precision, within the peer's
bandwidth budget.

Priority SHALL be scored from: distance from the peer's viewpoint, visibility, ownership, gameplay
importance flags, recency of change, and **staleness** — how long since the entity was last sent to
this peer.

Priority SHALL select a **network LOD band** determining update frequency and encoding precision:

| Band | Typical frequency | Precision |
|---|---|---|
| Owned or selected | 30–60 Hz | Full |
| Near, changing | 20 Hz | Full |
| Near, idle | 10 Hz | Reduced |
| Far | 2 Hz | Coarse |
| Very far | 0.2 Hz, or dormant | Minimal |

Clients SHALL interpolate at frame rate regardless of the update frequency received.

The scheduler SHALL guarantee **bounded staleness** per band, so a low-priority entity is never
starved indefinitely, and staleness SHALL feed back into the score.

Entities whose state has not changed MAY become **dormant**, sending nothing until they change,
with dormancy explicitly signalled so the client does not treat them as lost.

#### Scenario: Relevant exceeds budget
- **WHEN** 3,500 entities are relevant and the budget permits 800 updates
- **THEN** the scheduler SHALL select by priority, and the remainder SHALL be sent at lower
  frequency rather than dropped indefinitely

#### Scenario: Degradation before omission
- **WHEN** bandwidth pressure increases
- **THEN** distant entities SHALL first move to lower frequency and coarser precision, and only
  become dormant or omitted when that is insufficient

#### Scenario: No starvation
- **WHEN** an entity remains low priority for an extended period
- **THEN** rising staleness SHALL raise its score until it is sent within its band's guaranteed
  interval

#### Scenario: Dormancy is explicit
- **WHEN** a static entity stops sending updates
- **THEN** the client SHALL be informed it is dormant rather than inferring loss

### Requirement: Replication cells
Replicated state SHALL be organised into **replication cells**: a spatial partition used to
accelerate relevance queries, to scope baselines, and to coordinate with content streaming.

Replication cells SHALL derive from the **world partition** (see
`world-partition-and-streaming`) rather than from a partition owned by networking. Networking SHALL
NOT maintain a second spatial subdivision of the same world.

Networking MAY aggregate world cells into coarser replication cells where relevance granularity
differs from streaming granularity, but the aggregation SHALL be derived from world cell identity,
so client and server refer to the same content by the same identifiers.

A peer's interest SHALL be expressed as a set of cells derived from its viewpoint, and entering or
leaving a cell SHALL be an explicit event that other systems can observe.

Cell membership SHALL drive **asset preloading** and **world content streaming**, so that a client
entering a cell can have its content resident before entities arrive: the server's relevance
decision SHALL be able to issue a streaming hint to the client ahead of replicating the entities.

#### Scenario: Cell entry drives preloading
- **WHEN** a client's viewpoint approaches a new cell
- **THEN** the cell-entry event SHALL be observable so world and asset streaming can preload its
  content before replicated entities are spawned

#### Scenario: Relevance query is bounded
- **WHEN** relevance is computed for a peer in a large world
- **THEN** only entities in the peer's interest cells SHALL be considered, rather than all
  entities

#### Scenario: Partition source is replaceable
- **WHEN** the world partition's configuration changes
- **THEN** replication cells SHALL follow it without changes to network identity, interest
  scoring, or the scheduler

#### Scenario: One spatial model
- **WHEN** the server evaluates relevance and the client streams content
- **THEN** both SHALL refer to the same world cell identities, with no separate networking
  partition to keep in agreement

### Requirement: Bandwidth management
The engine SHALL enforce a per-peer outgoing bandwidth budget, packing updates into packets by
priority, tracking what each peer has acknowledged, and reporting utilisation.

Packets SHALL be sized to avoid IP fragmentation, with a configurable MTU.

Budget enforcement SHALL degrade in a defined order: reduce update frequency for low-priority
bands, reduce encoding precision, then mark entities dormant — before omitting updates entirely.

Budgets SHALL be settable per peer, so a peer on a constrained connection can be served
differently without changing the simulation.

#### Scenario: Budget enforcement
- **WHEN** more updates are pending than fit the budget
- **THEN** the highest-priority updates SHALL be sent and the rest deferred, with the backlog
  reported

#### Scenario: Degradation order
- **WHEN** a peer's budget is exceeded
- **THEN** frequency and precision SHALL be reduced before any entity stops being updated

#### Scenario: Per-peer budgets
- **WHEN** one peer has a much lower bandwidth budget
- **THEN** it SHALL receive a reduced update stream without affecting other peers or the
  simulation

### Requirement: Client prediction and reconciliation
Clients SHALL locally predict the results of their own inputs for entities they control, without
waiting for the server.

The client SHALL: buffer inputs with tick numbers, apply them immediately to the predicted state,
send them to the server, and retain them until acknowledged.

On receiving an authoritative state for a tick, the client SHALL compare it to its predicted state
for that tick; if they diverge beyond a tolerance, it SHALL **reconcile** by resetting to the
authoritative state and re-simulating the buffered inputs.

Reconciliation SHALL be smoothed visually so corrections do not appear as snaps.

#### Scenario: Local movement feels immediate
- **WHEN** a player moves under 100 ms of latency
- **THEN** their character SHALL respond immediately to input, not after a round trip

#### Scenario: Misprediction corrected
- **WHEN** the server's authoritative state diverges from the prediction
- **THEN** the client SHALL re-simulate from the authoritative state and blend the visual
  correction over a short period

#### Scenario: Deterministic re-simulation
- **WHEN** re-simulation runs
- **THEN** it SHALL use the same fixed-step schedule and system order as the original simulation

### Requirement: Rollback and reconciliation primitives
The engine SHALL provide rollback as a reusable mechanism rather than a per-game implementation.

It SHALL provide: an **input buffer** recording local inputs with tick numbers, **per-tick state
snapshots** over a bounded window using the ECS snapshot mechanism, a **comparison** of predicted
against authoritative state for a tick, and a **replay** that restores the authoritative state and
re-simulates buffered inputs.

Systems participating in rollback SHALL be **snapshot-restorable and deterministic**; systems that
are not — VFX, audio, non-pinned inference, adaptive controllers — SHALL be excluded from the
rollback set by declaration, and SHALL NOT be re-simulated during replay.

Corrections SHALL be **smoothed** for presentation over a configurable interval, so a rollback is
not visible as a snap.

The rollback window SHALL be bounded; an authoritative update older than the window SHALL cause a
full state resynchronisation rather than an unbounded replay.

#### Scenario: Misprediction is corrected
- **WHEN** authoritative state for tick N differs from the client's prediction beyond tolerance
- **THEN** the client SHALL restore tick N and replay buffered inputs to the present tick

#### Scenario: Non-deterministic systems are excluded
- **WHEN** a replay runs
- **THEN** VFX and audio SHALL NOT be re-simulated, so a rollback does not replay explosions or
  retrigger sounds

#### Scenario: Correction is not a snap
- **WHEN** a correction changes a character's position
- **THEN** the visual position SHALL converge over the smoothing interval while the simulation
  state is corrected immediately

#### Scenario: Beyond the window
- **WHEN** an authoritative update predates the rollback window
- **THEN** a full resynchronisation SHALL occur, reported as such

### Requirement: Interpolation and lag compensation
Entities a peer does not control SHALL be rendered **interpolated** between received states, with
a configurable interpolation delay trading latency against smoothness, and extrapolation bounded
to a short window when updates are late.

The server SHALL support **lag compensation** for hit detection: rewinding the state of relevant
entities to the tick the shooting client observed, within a bounded rewind window.

Lag compensation SHALL be implemented over a retained history of **collision proxies** —
simplified shapes and transforms per tick — rather than full physics state, and the resulting
accuracy difference from a live query SHALL be documented.

The rewind window SHALL be bounded and the client's claimed tick validated against its measured
latency, so the advantage available to a high-latency or malicious client is limited.

#### Scenario: Smooth remote players
- **WHEN** remote player updates arrive at 20 Hz and the client renders at 120 Hz
- **THEN** they SHALL be interpolated smoothly rather than stepping

#### Scenario: Lag-compensated hit
- **WHEN** a client fires at a target it saw at a past tick
- **THEN** the server SHALL rewind that target to that tick within the compensation window and
  evaluate the hit there

#### Scenario: Rewind window bounded
- **WHEN** a client claims a tick outside the compensation window
- **THEN** the claim SHALL be rejected, bounding the advantage a high-latency or malicious client
  can obtain

#### Scenario: Proxy accuracy is documented
- **WHEN** a rewound hit test uses collision proxies
- **THEN** the accuracy difference from a live query SHALL be documented, rather than presented as
  exact

### Requirement: Scene and level synchronisation
Peers SHALL be able to synchronise level state: the server directs peers to load a scene, and
peers report readiness so the server can start play only when everyone is loaded.

Networked prefabs SHALL be identified by stable ids consistent across builds, verified at
connection time so mismatched content is detected immediately.

#### Scenario: Content mismatch
- **WHEN** a client's networked prefab registry differs from the server's
- **THEN** the mismatch SHALL be detected at connection with a clear diagnostic, rather than
  causing corrupt spawns later

### Requirement: Dedicated server
The engine SHALL support a **dedicated server** configuration that excludes client-only subsystems
and content at build and cook time, rather than disabling them at runtime.

A dedicated server build SHALL exclude: the renderer, VFX, UI, client audio, and the editor. It
SHALL retain: ECS, physics, AI, navigation, animation to the extent gameplay requires it,
networking, and gameplay code.

A dedicated server cook SHALL exclude client-only assets — textures, high-resolution meshes,
audio, shaders, VFX assets — while retaining collision, navigation, and gameplay data, with the
exclusions reported.

Where a server needs a subset of an otherwise client-only asset (collision geometry from a mesh),
the cook profile SHALL retain that subset rather than the whole asset.

#### Scenario: Server carries no rendering
- **WHEN** a dedicated server build is produced
- **THEN** no graphics backend SHALL be linked and no textures or shaders SHALL be packaged

#### Scenario: Collision without the mesh
- **WHEN** a mesh contributes collision geometry
- **THEN** the server cook SHALL retain the collision representation without the render mesh

#### Scenario: Exclusions are reported
- **WHEN** a server cook completes
- **THEN** it SHALL report what was excluded and the resulting size, so accidental inclusions are
  visible

### Requirement: Security posture
The engine SHALL treat all client input as untrusted:

- the server SHALL validate every RPC's sender authority and parameter bounds
- the server SHALL validate client inputs for plausibility (rate, magnitude, timing)
- clients SHALL never be trusted for authoritative state, hit results, or resource amounts
- transports SHALL support encryption and authentication (DTLS for UDP, TLS for WebSocket)
- the connection handshake SHALL support an application-supplied authentication step before a
  peer is admitted

The documentation SHALL state plainly that client-side anti-cheat is out of scope and that
authority is the only real defence.

#### Scenario: Malicious input rate
- **WHEN** a client sends inputs faster than the tick rate allows
- **THEN** the server SHALL clamp or reject the excess and flag the peer

#### Scenario: Authentication before admission
- **WHEN** a peer connects with an authentication callback configured
- **THEN** it SHALL remain in a pending state until authentication succeeds or times out, without
  receiving game state

### Requirement: Gameplay and scripting API
Networking SHALL be exposed through ECS components and declarative annotations in Swift and C++,
with the scripting layer declaring intent and the engine performing scheduling, encoding, and
transport.

Scripts SHALL be able to declare: replicated components and their schemas, RPCs with direction,
delivery mode and authority requirements, relevance predicates, and priority contributions.

Network identity and ownership SHALL be components (`NetworkIdentity`, `ReplicationState`) rather
than a base class, so a replicated entity is an ordinary entity with additional components.

#### Scenario: No inheritance hierarchy
- **WHEN** an entity is made replicated
- **THEN** it SHALL gain components, not a base type, and existing systems querying it SHALL be
  unaffected

#### Scenario: Declarative RPC in Swift
- **WHEN** a developer declares a server RPC in Swift
- **THEN** the engine SHALL generate its serialisation, validate its authority on receipt, and
  route it, with no hand-written packet code

### Requirement: Network profiler
The engine SHALL provide a network profiler that answers **causal** questions, not only aggregate
ones.

It SHALL report: bandwidth in and out with a breakdown by component schema, by entity, and by
category (state, RPC, spawn, acknowledgement, overhead); round-trip time, jitter and loss; snapshot
sizes; entities sent, deferred and dormant per tick; prediction error magnitude and correction
frequency; and interest set sizes per peer.

It SHALL be able to answer, for a selected entity and peer: **why it was replicated** — which
relevance rule admitted it, what its priority score was, and which factors contributed; and for a
selected tick: **why a peer received a given byte count**, attributed down to schema and entity.

It SHALL be able to explain a **correction**: which field diverged, by how much, and the tick at
which the divergence originated.

A packet-level capture SHALL be recordable for offline analysis.

#### Scenario: Why was this replicated
- **WHEN** a developer asks why entity 821 was sent to a peer
- **THEN** the profiler SHALL name the relevance rule, the priority score, and the contributing
  factors

#### Scenario: Attributing bandwidth
- **WHEN** a client receives an unexpectedly large update
- **THEN** the breakdown SHALL attribute it to schemas and entities, not merely report a total

#### Scenario: Explaining a correction
- **WHEN** a player reports rubber-banding
- **THEN** the profiler SHALL show which field diverged, its magnitude, and the originating tick

### Requirement: Networking diagnostics
The engine SHALL provide: per-peer round-trip time, jitter, and loss; bandwidth in and out broken
down by entity, component schema, and RPC; prediction error magnitude and reconciliation
frequency; interest set and candidate set sizes; scheduler decisions including deferred and dormant
counts; and a packet-level log for offline analysis.

For `Lockstep` sessions it SHALL additionally report state hash comparison results and the tick of
any divergence.

#### Scenario: Finding a bandwidth hog
- **WHEN** bandwidth is unexpectedly high
- **THEN** the per-schema breakdown SHALL identify which replicated field dominates

#### Scenario: Diagnosing rubber-banding
- **WHEN** players report rubber-banding
- **THEN** the prediction error metrics SHALL show reconciliation frequency and magnitude,
  isolating whether the cause is latency, misprediction, or non-determinism

#### Scenario: Lockstep divergence
- **WHEN** a lockstep session diverges
- **THEN** the diagnostic SHALL report the tick and the mismatching hash, so the responsible
  system can be identified
