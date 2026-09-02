# networking-and-replication Specification

## Purpose

Defines multiplayer: the transport abstraction, the authority model, state replication over ECS,
remote procedure calls, client prediction and reconciliation, interest management, and the
security posture.

The model is **server-authoritative with client prediction** by default, because that is what
produces trustworthy, responsive multiplayer; peer-to-peer and listen-server topologies are
supported configurations of the same machinery.

## Requirements

### Requirement: Transport abstraction
`Transport` SHALL be the engine-defined interface for moving bytes between peers, providing:
connect, disconnect, send on a numbered channel with a delivery mode, receive, and connection
state and statistics (round-trip time, packet loss, bandwidth).

Delivery modes: `Unreliable`, `UnreliableSequenced`, `ReliableUnordered`, `ReliableOrdered`.

The engine SHALL ship: a **UDP transport** with its own reliability layer, a **WebSocket
transport** for browser targets, and a **local transport** for single-process testing. Additional
transports SHALL be implementable as modules.

#### Scenario: Channel independence
- **WHEN** a reliable chat message is delayed by retransmission
- **THEN** unreliable state updates on another channel SHALL not be head-of-line blocked

#### Scenario: Transport swapped for tests
- **WHEN** an integration test runs client and server in one process
- **THEN** the local transport SHALL be used with configurable simulated latency and loss, and no
  gameplay code SHALL change

### Requirement: Network simulation
The transport layer SHALL provide a **network condition simulator** applying configurable latency,
jitter, packet loss, duplication, and reordering, usable in development builds and tests.

#### Scenario: Testing under adverse conditions
- **WHEN** a developer enables 150 ms latency with 5 % loss
- **THEN** the game SHALL run against those conditions locally, exercising prediction and
  reconciliation

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

### Requirement: Component replication
Replication SHALL be declarative: components and fields marked `Replicated` are synchronised from
the authority to interested peers.

Per-field configuration SHALL include: a condition (always, on change, or a predicate), a
quantisation and precision specification, an update priority, and a target filter (everyone, the
owner only, or everyone except the owner).

Replication SHALL use ECS change detection so unchanged data is not sent, and SHALL **delta
encode** against the last state each peer acknowledged.

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

### Requirement: Interpolation and lag compensation
Entities a peer does not control SHALL be rendered **interpolated** between received states, with
a configurable interpolation delay trading latency against smoothness, and extrapolation bounded
to a short window when updates are late.

The server SHALL support **lag compensation** for hit detection: rewinding the state of relevant
entities to the tick the shooting client observed, within a bounded rewind window.

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

### Requirement: Interest management
The server SHALL determine, per peer, which entities are **relevant**, and replicate only those.

Relevance SHALL be computable by: distance from the peer's viewpoint, spatial partitioning, team
or faction membership, explicit always-relevant marking, and custom predicates.

Entities entering relevance SHALL receive a baseline; entities leaving SHALL be explicitly
dropped so the client can clean up.

#### Scenario: Large world
- **WHEN** a world contains 10 000 entities and a peer can perceive 200
- **THEN** only those 200 SHALL be replicated to that peer

#### Scenario: Relevance change
- **WHEN** an entity leaves a peer's relevance set
- **THEN** the peer SHALL be told to remove it, rather than being left with a frozen ghost

#### Scenario: Information leakage
- **WHEN** an entity is not relevant to a peer
- **THEN** no data about it SHALL be sent, so a modified client cannot observe it

### Requirement: Bandwidth management
The engine SHALL enforce a per-peer outgoing bandwidth budget, packing updates into packets by
priority, tracking what each peer has acknowledged, and reporting utilisation.

Packets SHALL be sized to avoid IP fragmentation, with a configurable MTU.

#### Scenario: Budget enforcement
- **WHEN** more updates are pending than fit the budget
- **THEN** the highest-priority updates SHALL be sent and the rest deferred, with the backlog
  reported

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

### Requirement: Scene and level synchronisation
Peers SHALL be able to synchronise level state: the server directs peers to load a scene, and
peers report readiness so the server can start play only when everyone is loaded.

Networked prefabs SHALL be identified by stable ids consistent across builds, verified at
connection time so mismatched content is detected immediately.

#### Scenario: Content mismatch
- **WHEN** a client's networked prefab registry differs from the server's
- **THEN** the mismatch SHALL be detected at connection with a clear diagnostic, rather than
  causing corrupt spawns later

### Requirement: Networking diagnostics
The engine SHALL provide: per-peer round-trip time, jitter, and loss; bandwidth in and out broken
down by entity, component, and RPC; prediction error magnitude and reconciliation frequency;
interest set sizes; and a packet-level log for offline analysis.

#### Scenario: Finding a bandwidth hog
- **WHEN** bandwidth is unexpectedly high
- **THEN** the per-component breakdown SHALL identify which replicated field dominates

#### Scenario: Diagnosing rubber-banding
- **WHEN** players report rubber-banding
- **THEN** the prediction error metrics SHALL show reconciliation frequency and magnitude,
  isolating whether the cause is latency, misprediction, or non-determinism
