# A secure transport, replication from the world, and the hosts that run them

## Why

Issue #82 found CyberNet's parts tested and unjoined. No transport a shipping build admitted
existed: `UdpTransport` has no encryption or peer authentication and `admissible_for(..., Shipping)`
refuses it. No `ecs::World` appeared in any networking signature, so replication read no change
detection and walked no chunk column. And nothing ran a server and a client together. The owner
chose online multiplayer as the program's next track; this change is its core, and the Swift API is
the next.

## What changes

- **`SecureTransport`**, a `Transport` over any `DatagramSocket` (`UdpSocket`, POSIX and Winsock; or
  `SimulatedSocket` on the seeded `LocalNetwork`): a three-message Noise-NK handshake with a
  stateless cookie challenge; XChaCha20-Poly1305 packets under per-direction keys with the header as
  associated data; a 64-bit replay window checked before decryption; fragmentation up to 255
  fragments; a token-bucket send rate cut on loss and delay; RTT, jitter and loss in each direction;
  `ConnectionEvent`s and `DisconnectReason`s. Cryptography is Monocypher's (already pinned for the
  shipping manifest), named by one translation unit.
- **`WorldReplicator` and `ReplicaWorld`**: replication from an `ecs::World` through
  `NetworkIdentity`, the world's chunk columns and change versions, per-peer relevance, the
  priority scheduler and an exact budget, exact acknowledgement, and repeated spawns and despawns;
  `schema_from_reflection()` derives schemas from `Replicated(...)` attributes on stable field ids.
- **`PeerBaseline::acknowledge_exactly()`** beside the cumulative `acknowledge()`, which promoted a
  lost snapshot's contents on a later acknowledgement.
- **`ServerHost` and `ClientHost`**: the join through the compatibility scope, the fixed tick, the
  command stream with rate, order and window validation, and `ServerInputLog` with `replay_step()`.
- **Tools**: `cy_net_server` (headless), `cy_net_client`, `cy_net_session_bench`,
  `cy_net_session_trace`; `just run-net-server`, `just run-net-client`, `just capture-net-session`.
- **Spec**: "Security posture" admits a Noise-pattern handshake over vetted primitives beside DTLS,
  and three requirements are added: the secure UDP transport, replication from an ECS world, and
  the hosts.

## What this is not

- Not client prediction joined to reconciliation, nor interpolation: the next step of #82.
- Not a Swift API: the next step of #82; the C++ surface is shaped for it.
- Not DTLS, WebSocket or IPv6, and not cryptographic client authentication: the client is admitted
  by the application's `CredentialCheck`.
