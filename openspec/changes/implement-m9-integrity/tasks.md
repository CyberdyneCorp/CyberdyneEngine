# Tasks: M9 — Integrity

Ordered. Section 0 is the spike and it runs first because the profile's definition depends on its
answer, and section 1 is the log because four of this milestone's five readers cannot be built
against a log that does not exist yet.

## 0. The spike — cross-platform floating-point determinism

- [ ] 0.1 Measure, do not assume: the engine's own math primitives across clang 18 and GCC 13 at
      `-O0`, `-O1`, `-O2` and `-O3 -flto`, bit-compared. A disagreement between two compilers on ONE
      machine is a finding about the profile's definition, not about platforms
- [ ] 0.2 The `<cmath>` transcendental set, same comparison. If it does not agree, the profile
      forbids them and the engine ships its own — and that cost is known before
      `simulation-and-determinism` claims Complete
- [ ] 0.3 **State plainly what this machine cannot answer.** It has one architecture, so the
      cross-architecture half of "lockstep holds across two platforms" is reported NOT EVALUATED
      through the ledger's own `requires`/`where` mechanism, never as a pass
- [ ] 0.4 Commit the spike and record its answer in `design.md`, the way M8.b's IR spike was
      committed and M8.c's design consumed it without re-deriving it

## 1. The command log — one record, written once

- [ ] 1.1 `cy::gameplay::CommandStream` grows a log seam: every command recorded once, in order,
      with its provenance, and **nothing about provenance affecting validation, ordering or
      execution** — `gameplay-framework` forbids it and M8.c's firewall decision depends on it
- [ ] 1.2 External results: everything the simulation consumed that it did not compute
- [ ] 1.3 Snapshot kinds and checkpoints, generalising `cy::ecs::Snapshot` rather than replacing it
- [ ] 1.4 **The side-effect ledger** — what must not be applied twice when a rollback re-simulates
- [ ] 1.4b **Adopt the determinism firewall M8.c built.** It is armed and guards nothing: the only
      callers of `declare`/`declare_from_reflection` in the tree are test files, so a game built on
      this engine today has a firewall that refuses nothing because it has been told nothing is
      authoritative. Replication is the capability that knows which components are on the wire, so
      this is the milestone that can answer it — and the check is a startup report (`guarded_count`
      beside `AuthorityDerivationReport::underived`) asserted by an artefact, not by a unit test
      with three components in it
- [ ] 1.5 A test that the five readers read ONE record type. A reader that grew its own is the
      failure §2 of `design.md` names, and it should be a check rather than a review

## 2. Determinism — `simulation-and-determinism` → C

- [ ] 2.1 Determinism profiles, declared by subsystems and required by sessions
- [ ] 2.2 **Rejected at configuration**: a session declaring a profile a subsystem cannot meet fails
      when it is configured, naming the subsystem and the guarantee — not diagnosed later. This is
      the same shape as M8.c's cook-time refusal and is picked for the same reason
- [ ] 2.3 Deterministic parallelism, stable iteration, the floating-point policy from section 0
- [ ] 2.4 Generated state codecs and hierarchical hashing
- [ ] 2.5 **The validator**: two state hashes and a log in, one field on one entity out
- [ ] 2.6 **The determinism lint**: the static half, finding the code that could not meet the profile

## 3. Replay and rollback — `replay-and-rollback` → W

- [ ] 3.1 Playback and seeking against checkpoints; presentation tracks
- [ ] 3.2 Rollback re-simulates from a checkpoint without re-applying ledgered side effects,
      **proven by a duplicate-effect test** — the exit criterion says "proven", so the test is the
      criterion and it must be shown to fail with the ledger removed
- [ ] 3.3 Lockstep and resynchronisation
- [ ] 3.4 The crash replay buffer, bounded, and flushed into the crash artefact

## 4. Networking — `networking-and-replication` → W

- [ ] 4.1 **`CY_NETWORKING` stops gating nothing.** It defaults ON with `src/networking/` behind it,
      and the tree builds and passes with it OFF. M8.b shipped `CY_UI` tested in neither direction
      and M8.c's gate had to check every option it added in both — this milestone starts there
- [ ] 4.2 Three network modes and the authority model; transports
- [ ] 4.3 Replication schemas, component replication, baselines and deltas, spawning, RPCs
- [ ] 4.4 Interest management, priority scheduling and bandwidth budgets — **measured as a curve
      against entity count**, which is the exit criterion's own word, not asserted at one population
- [ ] 4.5 Prediction, reconciliation and lag compensation
- [ ] 4.6 The dedicated server, headless

## 5. The three contingent Complete rows

- [ ] 5.1 `gameplay-framework` → C: network integration, save and replay contracts, headless
      operation, performance contracts. **If sections 3 and 4 do not land, this row does not move**,
      and `design.md` §4 says so at proposal time rather than at the gate
- [ ] 5.2 `save-and-persistence` → C: integrity and confidentiality, storage backends, checkpoints
- [ ] 5.3 `diagnostics-profiling-and-crash` → C: rolling capture, crash artefacts, breadcrumbs,
      reproduction artefacts, remote and server diagnostics, telemetry export

## 6. The artefact — `samples/09-multiplayer`

- [ ] 6.1 A four-player session over a simulated adverse network: prediction, reconciliation and
      rollback under packet loss. **A second sample, and `design.md` §4 states why**: the subject is
      what happens between four processes
- [ ] 6.2 The session recorded and replayed bit-exactly, including after seeking
- [ ] 6.3 A deliberately injected divergence narrowed to one field on one entity, with the artefact
      that reproduces it
- [ ] 6.4 A crash produces an artefact that reproduces the crash from the replay buffer
- [ ] 6.5 **Capture it.** M8.c made this project's first photographed artefact and the rule holds:
      anything with a visible result gets an image under `docs/design/images/`, and a diagram is
      labelled one

## 7. Records and gates

- [ ] 7.1 `tools/roadmap/milestones/m9.toml`; declare `milestone-m9` in `gates.toml` and raise
      `selftest.MINIMUM_CRITERIA`
- [ ] 7.2 An `m10-open` criterion using the double-star glob form
- [ ] 7.3 Update `status.yaml`, `capability-matrix.md`, `ROADMAP.md` and `dependencies.md`
- [ ] 7.4 Move `ci.yml`'s milestone job to `m9` in the same commit that flips the gate green
- [ ] 7.5 Open the M10 change
- [ ] 7.6 **Two record corrections M8.c's closing gate handed forward, each an OpenSpec change
      against `delivery-roadmap` rather than an edit to a document.** First, its milestone table
      gives M8's artefact as "a playable vertical-slice game exercising every gameplay-facing
      capability at Working", and `vfx-system` exits M8.c at **Seed** with its `W` cell at M10 —
      the sentence needs correcting, and correcting it is not the same as claiming the tier.
      Second, `docs/roadmap/capability-matrix.md`'s three "The status record" lists have been
      hand-maintained since M7 and are two milestones stale; the honest fix is to generate them from
      `tools/roadmap/record.py`, which M8.c recorded as a finding and did not perform

## 8. The gate

- [ ] 8.1 Clean build of every profile from empty; `test-all` in each; every gate by hand
- [ ] 8.2 **Every criterion executes something and can fail** — break what it checks and prove it
      goes red. Twelve gates in a row have found a claim exceeding what was checked, and fifteen of
      eighteen findings were in the CHECKING rather than the engine
- [ ] 8.3 Adversarial pass: remove the side-effect ledger and confirm the duplicate-effect test goes
      red; inject a divergence and confirm the validator localises it; run with `CY_NETWORKING` OFF
- [ ] 8.4 Records verified against what the code supports. **`design.md` §4 predicts the demotion**:
      if one is needed it is `networking-and-replication`, and it should be split rather than
      claimed thin
