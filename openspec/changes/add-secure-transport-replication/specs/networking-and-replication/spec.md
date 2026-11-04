## MODIFIED Requirements

### Requirement: Security posture
The engine SHALL treat all client input as untrusted:

- the server SHALL validate every RPC's sender authority and parameter bounds
- the server SHALL validate client inputs for plausibility (rate, magnitude, timing)
- clients SHALL never be trusted for authoritative state, hit results, or resource amounts
- transports SHALL support encryption and authentication through an established protocol design
  over vetted primitives — DTLS, or a Noise-pattern handshake with an authenticated cipher, for UDP;
  TLS for WebSocket — and SHALL NOT use a hand-written cipher
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

## ADDED Requirements

### Requirement: Secure UDP transport
The engine SHALL ship a transport over UDP that authenticates the server to the client during a
connection handshake, derives fresh per-direction session keys for every connection, and seals
every packet after the handshake with an authenticated cipher whose associated data covers the
packet header. The handshake SHALL keep no server state until the client has answered a stateless
challenge from its own address, and the client's first message SHALL be no smaller than the
server's answer to it. A packet whose counter has already been accepted, or is older than the replay
window, SHALL be refused before it is decrypted. The transport SHALL carry messages larger than one
datagram by fragmenting them on their own channel and delivery mode, delivering an unreliable
message only when every fragment arrived; SHALL limit its send rate with a budget that responds to
measured loss and delay; SHALL measure round trip, jitter and loss in each direction; and SHALL end
a connection with a named reason. It SHALL report itself encrypted and authenticated only when its
keys come from the operating system's cryptographic generator.

#### Scenario: A tampered packet is refused
- **WHEN** one bit of a sealed packet's ciphertext or header is changed in transit
- **THEN** the receiver refuses it, delivers nothing from it, counts it, and keeps the connection

#### Scenario: A replayed packet is refused
- **WHEN** an authentic packet is sent to the receiver a second time, byte for byte
- **THEN** it is refused before decryption and its payload is delivered once

#### Scenario: The wrong server is not trusted
- **WHEN** a client trusts one server key and another server answers its handshake
- **THEN** the client refuses the answer, never connects, and reports a handshake timeout

#### Scenario: Shipping admission
- **WHEN** the secure transport runs with the operating system's entropy
- **THEN** `admissible_for(..., Deployment::Shipping)` admits it, and with a seeded entropy source it
  refuses it as not encrypted

### Requirement: Replication from an ECS world
The server SHALL replicate from an `ecs::World` by walking the chunk columns of every entity that
carries a network identity component, reading the world's per-chunk change versions so that an
entity whose replicated columns are older than the peer's acknowledged baseline is neither compared
nor sent. Marking an entity replicated SHALL add components without changing any existing query's
matches. The client SHALL decode snapshots into its own world, bound to components by reflected
type, creating and destroying entities as spawns and despawns arrive and never applying a snapshot
older than one it has applied. Acknowledgement SHALL be exact: a baseline SHALL be promoted, a spawn
confirmed and a despawn retired only for the snapshot that carried it, and spawns and despawns SHALL
repeat until such a snapshot is acknowledged. A snapshot SHALL never exceed the peer's budget,
despawns included.

#### Scenario: An unchanged world costs nothing
- **WHEN** nothing in the server's world has been written since a peer acknowledged its baselines
- **THEN** that peer's next snapshot holds only its header and no entity is compared

#### Scenario: A change in a lost snapshot is sent again
- **WHEN** the snapshot carrying a change is lost and a later snapshot that did not carry it is
  acknowledged
- **THEN** the change is sent again and the client converges to the server's value

### Requirement: Dedicated-server and client hosts
The engine SHALL provide a headless dedicated-server host that admits clients by their
compatibility scope, runs the application's simulation at a fixed tick with the gameplay commands
due at it, and replicates its world to each admitted client within that client's budget; and a
client host that joins, applies and acknowledges snapshots, and sends commands. Every join, leave
and applied command SHALL be recorded with the tick it was applied at, and feeding that record to a
server over a fresh world SHALL reproduce the recorded world at every tick.

#### Scenario: Clients converge under a bad network
- **WHEN** a server and two clients run over a 200 ms round trip with 5 % loss and reordering and the
  server's world comes to rest
- **THEN** both clients' replicated state equals the server's

#### Scenario: The server is deterministic under the same input
- **WHEN** a recorded session's input log is replayed into a server with no network
- **THEN** the replayed world equals the recorded world at every tick

#### Scenario: An incompatible client receives no state
- **WHEN** a client whose schema set differs joins
- **THEN** it is refused naming the schema-set mismatch and receives no snapshot
