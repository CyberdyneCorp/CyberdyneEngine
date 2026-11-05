# Tasks — secure transport, replication from the world, the hosts

## 1. The transport

- [x] 1.1 `crypto.h`: X25519, XChaCha20-Poly1305 and BLAKE2b from Monocypher; system and seeded entropy
- [x] 1.2 `DatagramSocket`: `UdpSocket` (POSIX, Winsock) and `SimulatedSocket`
- [x] 1.3 The NK handshake with a stateless cookie; sealed data packets; the replay window
- [x] 1.4 Fragmentation and reassembly; the send-rate controller; RTT and loss estimation
- [x] 1.5 Timeouts, disconnect frames and reasons; connection events; the credential check

## 2. Replication from a world

- [x] 2.1 `NetworkIdentity`; schemas from `Replicated(...)` attributes
- [x] 2.2 `WorldReplicator`: gather by chunk, skip by change version, relevance, scheduler, exact budget
- [x] 2.3 Exact acknowledgement (`PeerBaseline::acknowledge_exactly`), repeated spawns and despawns
- [x] 2.4 `ReplicaWorld`; `replicated_digest()`

## 3. The hosts

- [x] 3.1 `ServerHost`: join through the compatibility scope, the fixed tick, snapshots per peer
- [x] 3.2 Commands: encode, validate (rate, order, window), apply, `ServerInputLog`, `replay_step()`
- [x] 3.3 `ClientHost`: join, apply, acknowledge, submit
- [x] 3.4 `cy_net_server`, `cy_net_client`, `cy_net_session_bench`, `cy_net_session_trace`, the recipes

## 4. Evidence

- [x] 4.1 unit.networking_core: RFC 7748's X25519 vector, sealing, the window, fragments, the rate, the estimators
- [x] 4.2 integration.networking_session: attacks, loss, replication over worlds, the hosts at 200 ms and 5 % loss, determinism, refusals, budgets
- [x] 4.3 integration.networking_udp: the session on the loopback, admitted for shipping
- [x] 4.4 Mutations in evidence/falsification.txt, each red
- [x] 4.5 src/networking/README.md, docs/guides/networking.md, requirements-coverage.toml, the bandwidth figure and the session video
