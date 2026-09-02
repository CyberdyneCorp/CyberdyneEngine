# Expand networking into CyberNet

## Why

The `networking-and-replication` specification describes a competent
server-authoritative model: transports, authority, declarative component replication, RPCs,
prediction, interest management, and a bandwidth budget. It is roughly the shape of a good
mid-sized-game networking layer.

Three things are missing that this engine specifically needs.

**Only one architecture is supported.** Snapshot replication with client prediction is right for
action games and wrong for a large RTS, where sending ten thousand unit states is absurd compared
to sending a handful of commands. The engine has spent considerable effort establishing
determinism contracts in physics, AI, and animation; **deterministic lockstep** is what that
investment pays for, and it is currently unreachable because no network mode uses it.

**Interest management has no scheduler.** Relevance is specified as a boolean, but at RTS scale the
question is not "is this relevant" but "which 800 of these 3,500 relevant entities do I send this
tick, and at what fidelity". That is a priority-scheduled budget problem, matching the pattern
already established for VFX, audio, AI, and animation — and it is the difference between a
networking layer that scales and one that does not.

**Observability is thin.** Networking defects are almost always "why did this client receive
that", and the current diagnostics cannot answer it.

## What Changes

- **Three network modes**, declared per world: `SnapshotAuthoritative` (the current model),
  `Rollback` (prediction with full state rollback and replay), and `Lockstep` (deterministic
  command replication). The engine SHALL state precisely what each requires and guarantees.
- **Replication schemas** as first-class declarations: per-component wire descriptions with
  per-field quantisation, encoding, conditions, and priority, compiled to serialisation code
  rather than reflected per field per entity.
- **Snapshot and delta replication** with explicit baselines, acknowledgement tracking, change
  masks, and a documented compression toolkit — bit packing, quantised vectors and quaternions,
  dictionary and string tables, run-length encoding.
- **Priority-scheduled interest management with network LOD**: relevance produces candidates,
  a scheduler scores and selects within a bandwidth budget, and update frequency and precision
  degrade with priority rather than updates being simply dropped.
- **Replication cells** — a spatial partition of replicated state, with the contract for
  integrating world and asset streaming specified.
- **Rollback primitives**: recorded input, per-tick state snapshots, deterministic replay, and
  the reconciliation loop provided by the engine rather than reimplemented per game.
- **Lag compensation** built on recorded historical state, with the physics interaction specified.
- **A dedicated server profile**: a cook profile and build configuration that strips client-only
  content and subsystems.
- **A network profiler** that answers causal questions: why an entity was replicated, why a client
  received a given number of bytes, what caused a correction.
- **Authority migration seams** so distributed simulation servers remain possible later without
  reworking network identity.

Non-goals: matchmaking and lobby services, voice chat, and platform online services. These are
specified as integration points, not implemented.

## Capabilities

### Modified Capabilities

- `networking-and-replication` — substantially expanded: network modes, schemas, snapshots and
  delta, priority scheduling and network LOD, replication cells, rollback, lag compensation,
  dedicated server, profiler, and authority migration.
- `physics` — the determinism requirement gains an explicit statement of what lockstep requires
  and why cross-platform lockstep is not currently supported.
- `asset-import-pipeline` — cook profiles, including a dedicated server profile.
- `build-system-and-platforms` — a dedicated server build configuration.
- `thirdparty-dependencies` — transport and crypto integration recorded; platform services and
  voice recorded as integration points rather than engine features.

## Impact

- **Dependencies**: adds a transport backend (a QUIC or reliable-UDP library, to evaluate) and
  reuses the existing crypto dependency. Platform online services and voice remain out of tree.
- **Determinism**: `Lockstep` mode makes the engine's determinism contracts load-bearing. Its
  requirements are stated precisely, including the honest limitation that cross-platform lockstep
  is **not supported**, because the physics backend does not guarantee cross-platform
  reproducibility and the engine has no fixed-point or soft-float strategy.
- **World streaming**: replication cells need a spatial partition. The engine has **no world
  partition capability**; this change specifies the integration contract and records the gap.
- **Build**: a dedicated server configuration affects cooking, packaging, and feature selection.
