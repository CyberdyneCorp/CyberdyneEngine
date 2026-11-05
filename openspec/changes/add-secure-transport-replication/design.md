# Design — secure transport, replication from the world, the hosts

## Why a Noise NK handshake over Monocypher, and not DTLS

"Security posture" named DTLS. A DTLS stack is an OpenSSL- or mbedTLS-sized dependency with
certificates, a record layer and its own retransmission, and it would sit beneath a reliability layer
the engine already has. What the requirement asks for is confidentiality, server authentication,
replay protection and an application authentication step before admission. Noise's NK pattern
answers those with three primitives — X25519, XChaCha20-Poly1305 and BLAKE2b — all in Monocypher,
which `deps/manifest.toml` already pins (audited, one C file) for the shipping manifest's
signatures. The engine writes no cipher: it composes audited ones in a published pattern, and the
spec now says "an established protocol design over vetted primitives" rather than naming one
protocol.

The client knows the server's static public key in advance (shipped with the game or handed out by
a matchmaker); the client is admitted by its credential, which the application checks. Session keys
mix the static and the ephemeral Diffie-Hellman results, so a later leak of the server's static key
does not open recorded sessions while the server's cookie secret — which never leaves the process —
stays secret.

## Why the server keeps no state until RESPONSE

A HELLO flood is the cheapest attack on a UDP server. The cookie is a keyed BLAKE2b tag over the
client's address, ephemeral key and issue second; the server's ephemeral secret is derived from the
same inputs. RESPONSE carries the cookie back, and the server rebuilds the handshake from it. HELLO
is padded to 192 bytes and CHALLENGE is 105, so a spoofed HELLO cannot amplify.

## Why acknowledgement is exact

Snapshots are unreliable and each is acknowledged with the newest applied and a 32-bit mask. A
cumulative promotion — everything sent at or before the acknowledged snapshot — takes a lost
snapshot's contents for delivered; the next delta then omits the change and nothing sends it again.
`PeerBaseline::acknowledge_exactly()` promotes only what the named snapshot carried, and the
replicator's own records (spawns, despawns, the world version a baseline was taken at) follow the
same rule.

## Why the change version is per entity, not per peer

A chunk's column version says when it was last written. The replicator keeps, per peer and per
entity, the world version at which that peer's acknowledged baseline was taken. A per-peer "last
acknowledged snapshot's version" would be wrong the first time the budget left an entity out of a
snapshot that was then acknowledged: its chunk would look older than the acknowledgement while the
peer still lacked its change.

## Why the command rate is counted by the tick a command names

A client issues at most `max_commands_per_tick` per tick. Counted by arrival, a burst that loss and
retransmission bunched together would be refused as flooding; counted by the tick the command
names, with that tick required never to go backwards for one peer, an honest client is never
refused and a flooding one is clamped.
