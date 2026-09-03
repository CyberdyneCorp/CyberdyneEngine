# Tasks: simulation integrity

Specification-stage change. Sections 1 and 2 are the work of this change and are complete; the
change is archived on that basis. Sections 3 onward are the implementation backlog, sequenced by the
phase table in `design.md`.

## 1. Specification

- [x] 1.1 Record in `design.md`: determinism as a declared profile rather than a promise, the commit
      boundary as the moment everything keys off, epochs because ticks rewind, the ban on worker
      identity in ordering keys, the floating-point policy stated rather than wished, the firewall
      generalised with taint tracking, one command log for three consumers, external results for
      what cannot be reproduced, three snapshot encodings sharing one schema, the side-effect ledger,
      the save as the world overlay rather than a second model, dirty tracking and cell-scoped
      persistence so a save does not scan the world, atomic generations, hierarchical hashing with a
      chaos scheduler as the differentiator, the phase table, and the non-goals
- [x] 1.2 New `simulation-and-determinism` (20 requirements): clock, epochs, commit boundary,
      determinism profiles, deterministic parallelism, stable iteration and tie-breaking, the
      floating-point policy, random streams and their inspection, the determinism firewall, state
      classification, generated codecs, state providers, hierarchical hashing, the validator and
      chaos scheduling, determinism lint, registration order, diagnostics, performance and testing,
      and forbidden patterns
- [x] 1.3 New `replay-and-rollback` (19 requirements): one command log, external results, replay
      contents and compatibility, snapshot kinds, checkpoint policy, playback and seeking,
      presentation tracks, the rollback mechanism, the side-effect ledger, speculative versus
      confirmed effects, lockstep frames, resynchronisation, server recording, privacy, the crash
      replay buffer, validation, performance, and forbidden patterns
- [x] 1.4 New `save-and-persistence` (20 requirements): the overlay as the save, scopes, traits,
      identity, entity deltas and tombstones, dirty tracking, saving an unloaded world, the journal,
      consistent snapshots and background writing, container and manifest, atomic generations, the
      load pipeline, compatibility and migration, plugin state, integrity and confidentiality,
      backends and the cloud boundary, checkpoints, diagnostics, performance and testing, and
      forbidden patterns
- [x] 1.5 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 **Nine local determinism statements become one system.** Physics, networking lockstep, AI,
      VFX and inference firewalls, the job scheduler's deterministic mode, gameplay's clock and
      streams, and the world overlay were each locally correct and collectively unreconciled. The
      determinism profile is now the single decision from which each follows.
- [x] 2.2 `gameplay-framework` — the clock, random streams, and replay contracts delegate their
      mechanisms while keeping the gameplay-side declarations; epoch is added to moment identity
- [x] 2.3 `core-jobs-and-concurrency` — chaos scheduling added, and the ban on worker identity in
      ordering keys stated where commit ordering is defined
- [x] 2.4 `networking-and-replication` — the rollback *mechanism* moves to `replay-and-rollback`
      while networking keeps the policy, and effect suppression is delegated to the ledger rather
      than reimplemented
- [x] 2.5 `physics` — one guarantee becomes a declared policy, with the cross-platform lockstep case
      resolved: either physics is non-authoritative and movement uses deterministic math, or the
      configuration is rejected
- [x] 2.6 `core-math` — the floating-point policy per profile, and deterministic math types as an
      optional module that does not replace the library
- [x] 2.7 `world-partition-and-streaming` — the overlay gains the requirement that unloaded regions'
      persistent state is available without loading them, and that deltas apply during activation
- [x] 2.8 `testing-and-quality` — determinism verification per profile, across worker counts and
      chaos scheduling, golden replays, replay and save fuzzing, and transactional save tests
- [x] 2.9 `thirdparty-dependencies` — the simulation integrity layer recorded as engine-built, with
      codecs and cryptography integrated beneath it
- [x] 2.10 `ai-system`, `vfx-system`, `ml-inference`, `animation-and-skinning`, `audio`,
      `camera-system` — reviewed; no change needed. Their determinism and firewall statements are
      consistent with the generalised firewall and are consumed by it rather than contradicted.
- [x] 2.11 **Non-goals recorded**: backward simulation, replay compatibility across arbitrary code
      change, cross-platform bitwise determinism for arbitrary float code, and cloud save transport

## 3. Phase 1–3 — the foundation (deferred)

- [ ] 3.1 Tick, epoch, exact rational rate, bounded catch-up
- [ ] 3.2 The commit boundary and named tick phases
- [ ] 3.3 Determinism profile declaration and configuration-time validation
- [ ] 3.4 Counter-based random streams with hierarchical derivation and tracing
- [ ] 3.5 Stable tie-breaking conventions; query ordering declarations
- [ ] 3.6 Deterministic structural commit, event merge and reductions with logical ordering keys

## 4. Phase 4–5 — state and rollback (deferred)

- [ ] 4.1 State classification extended for simulation; generated snapshot and hash codecs
- [ ] 4.2 State providers for session, rules, teams, participants and random streams
- [ ] 4.3 Hierarchical state hashing with incremental subtree maintenance
- [ ] 4.4 Rollback snapshot ring, restore and re-simulation
- [ ] 4.5 The side-effect ledger; speculative and confirmed effect classification

## 5. Phase 6–7 — replay (deferred)

- [ ] 5.1 Command log recording with simulation points and sequence numbers
- [ ] 5.2 Replay container, compatibility manifest, chunked commands
- [ ] 5.3 Playback as a control source; headless fast-forward
- [ ] 5.4 Checkpoints, index, seeking policy
- [ ] 5.5 External result recording and consumption, with unrecorded-source detection

## 6. Phase 8–10 — save (deferred)

- [ ] 6.1 Persistence traits and scopes; persistent entity deltas and tombstones
- [ ] 6.2 The persistent state store for unloaded regions
- [ ] 6.3 Save container, manifest, chunking, journal and compaction
- [ ] 6.4 Consistent snapshot at the commit boundary; background serialisation
- [ ] 6.5 Dirty tracking that survives streaming
- [ ] 6.6 Atomic writes, generations, verification and fallback
- [ ] 6.7 Load pipeline applying deltas during cell activation
- [ ] 6.8 Migration on value records; structured load failures; plugin-owned state
- [ ] 6.9 Save and replay inspectors; semantic save diff

## 7. Phase 11–13 — validation and scale (deferred)

- [ ] 7.1 Determinism validator with chaos scheduling and divergence capture
- [ ] 7.2 Determinism lint in the build
- [ ] 7.3 Taint tracking for firewall violations
- [ ] 7.4 Lockstep profile: frames, input delay, deadline policies, hash exchange
- [ ] 7.5 Deterministic math module for authoritative paths
- [ ] 7.6 Resynchronisation with epoch increment
- [ ] 7.7 Crash replay buffer and its attachment to reports
- [ ] 7.8 Golden replays in continuous integration

## 8. Benchmarks and validation (deferred)

- [ ] 8.1 **Strategy determinism benchmark**: 8 participants, 100 000 units, 5 000 agent groups, 60
      ticks per second for minutes, across 1, 8 and 16 workers with randomised stealing, identical
      final hashes under `Lockstep`
- [ ] 8.2 **Rollback action benchmark**: 4 players, 60 hertz, a 32-tick window, injected latency,
      reordering and duplication — verifying convergence and no duplicated side effects
- [ ] 8.3 **Large-world save benchmark**: over a million persistent objects, 95 % unloaded, tens of
      thousands of dirty records — no world-wide load, no full scan, bounded main-thread cost
- [ ] 8.4 **Replay benchmark**: a multi-hour session — file size, seek time to a late tick,
      fast-forward rate, checkpoint memory
- [ ] 8.5 Transactional save failure injection after every write phase
- [ ] 8.6 Replay and save fuzzing
- [ ] 8.7 Cross-platform hash agreement for projects declaring `CrossPlatform`
- [ ] 8.8 Firewall tests: a deterministic system reading presentation data is reported
- [ ] 8.9 Ledger tests: a rolled-back and re-simulated tick realises each effect once

---

**Archived 2026-09-02.** Sections 1 and 2 are complete: `simulation-and-determinism`,
`replay-and-rollback` and `save-and-persistence` are in `openspec/specs/`, and eight capabilities
were updated. The change's main finding was reconciliation rather than addition — nine capabilities
had each made a locally correct determinism statement, and the determinism profile is now the single
decision from which each follows. The unchecked items from section 3 onward are the implementation
backlog; **phase 1, the commit boundary, is the milestone that matters**, because every later phase
keys off it and a system built without one has no defined moment at which its state can be captured.
