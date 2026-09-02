# Tasks: CyberNet

Specification-stage change. Sections 1 and 2 are the work of this change and are complete; the
change is archived on that basis.

Sections 3 to 8 record the implementation the decision implies and are **deliberately deferred to
implementation changes**. The priority scheduler (4.2) and the three simulation contracts
(section 5) are where the risk concentrates; see `design.md`.

## 1. Specification

- [x] 1.1 Record rationale, the lockstep limitation, and the world partition gap in `design.md`
- [x] 1.2 Expand `networking-and-replication`: network modes, lockstep requirements and limits,
      replication schemas, snapshots and delta, wire compression, priority scheduling and network
      LOD, replication cells, rollback primitives, dedicated server, authority migration seams,
      network profiler, and the scripting API
- [x] 1.3 Modify component replication, interest management, interpolation and lag compensation,
      bandwidth management, transport abstraction, and diagnostics to match
- [x] 1.4 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 `physics` — determinism requirement now states that its same-platform guarantee is what
      bounds lockstep, and that cross-platform lockstep would need a fixed-point or soft-float path
- [x] 2.2 `asset-import-pipeline` — cook profiles added, including `DedicatedServer` and the rule
      that a server retains subsets of otherwise client-only assets
- [x] 2.3 `build-system-and-platforms` — dedicated server build configuration, orthogonal to
      Debug/Development/Profile/Shipping, failing if a client-only subsystem is linked
- [x] 2.4 `thirdparty-dependencies` — networking recorded as engine-built above the transport;
      transport library recorded as an evaluation; platform online services and voice recorded as
      integration points, not engine features
- [x] 2.5 `ecs-core` — reviewed; no requirement change needed. Rollback uses the world snapshot
      mechanism already specified, and replication uses existing change detection.
- [x] 2.6 `ai-system`, `animation-and-skinning` — reviewed; no change needed. Their determinism
      requirements are exactly what `Lockstep` and `Rollback` modes depend on.
- [x] 2.7 **Gap recorded, not closed**: the engine has no world partition capability. Replication
      cells are networking-owned and the integration contract is specified so a future world
      partition can supply the partition instead. This should be its own change.

## 3. Transport and session (deferred to implementation)

- [ ] 3.1 Transport interface, channels, delivery modes, statistics
- [ ] 3.2 Reliable-UDP backend, or QUIC — evaluate against latency, encryption, and dependency cost
- [ ] 3.3 Encryption, authentication, replay protection, sequence validation, rate limiting
- [ ] 3.4 Local transport with network condition simulation
- [ ] 3.5 Session lifecycle: listen server, dedicated server, connection, reconnection, migration
      hooks

## 4. Replication (deferred to implementation)

- [ ] 4.1 Replication schemas: declaration, cook-time validation against reflection, compiled
      encoders, version verification at connect
- [ ] 4.2 Interest management with the priority scheduler, network LOD bands, staleness bounds,
      and dormancy
- [ ] 4.3 Snapshot and delta with baselines, acknowledgement tracking, change masks
- [ ] 4.4 Wire compression toolkit and per-field size reporting
- [ ] 4.5 Replication cells and the streaming integration contract
- [ ] 4.6 Entity spawning, lifetime, and networked prefab registry verification

## 5. Simulation contracts (deferred to implementation)

- [ ] 5.1 `SnapshotAuthoritative` mode
- [ ] 5.2 `Rollback` mode: input buffer, per-tick snapshots, comparison, replay, smoothing,
      exclusion of non-deterministic systems
- [ ] 5.3 `Lockstep` mode: command replication, tick synchronisation, state hashing, participant
      verification
- [ ] 5.4 Mode prerequisite verification at startup and session start
- [ ] 5.5 Lag compensation over retained collision proxies, with bounded rewind and claim
      validation

## 6. Dedicated server (deferred to implementation)

- [ ] 6.1 Build configuration and link-time exclusion, with a check that fails on client-only
      subsystems
- [ ] 6.2 `DedicatedServer` cook profile and asset subsetting
- [ ] 6.3 Headless operation, logging, and remote administration hooks

## 7. Tooling (deferred to implementation)

- [ ] 7.1 Network profiler: bandwidth attribution by schema and entity, RTT, loss, corrections
- [ ] 7.2 Causal queries: why an entity was replicated, why a byte count, what caused a correction
- [ ] 7.3 Packet capture and offline analysis
- [ ] 7.4 Schema authoring with size reporting

## 8. Validation (deferred to implementation)

- [ ] 8.1 Replication correctness: client state converges to server state under loss and reorder
- [ ] 8.2 Rollback tests: prediction, divergence, replay produces the authoritative result
- [ ] 8.3 Lockstep determinism tests across a matrix of participants on the supported scope
- [ ] 8.4 Interest and scheduler tests: bounded staleness, no starvation, budget respected
- [ ] 8.5 Security tests: unauthorised RPC rejection, out-of-bounds parameters, replay attacks,
      rewind claims outside the window
- [ ] 8.6 Benchmarks: 100 / 1,000 / 10,000 replicated entities across modes, as regression guards
