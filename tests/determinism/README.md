# `tests/determinism/`

Reproducibility of simulation and replication: the same inputs producing the same state, hashed per
tick. Budget: 10 s per test.

**Empty until M10**, and the delay is recorded rather than tidied away. `tests/CMakeLists.txt` used
to say the kind "joins them at M9", and M9 is the milestone that built the simulation this suite is
about — but it declared its determinism cases as `unit` and `integration`, which is a defensible
reading of a taxonomy whose other four rows are graded by COST, and which left the one row graded by
SUBJECT with nothing in it. `m9`'s closing gate said so in as many words and handed the empty
directory forward; M10 task 6.4 is where it is repaired.

## What is declared here

| Suite | What it is |
|---|---|
| `determinism.golden_replay` | A recorded session with **committed hashes**, replayed. `golden/toy-session-v1.cyreplay` is the log; `golden/toy-session-v1.hashes` is the state hash after every one of its 120 ticks. Nothing in the tree did this before: `src/replay/tests/test_bitexact.cpp` records and replays in one process, so a simulation that started doing something else moves both sides of its comparison at once and it stays green. |
| `determinism.replay_fuzz` | `read_log()` over a replay truncated at every length and corrupted at every byte. The invariant is two-sided — either refused **with a reason**, or accepted as a log that hashes to exactly what was written — because a reader that refused everything would satisfy the one-sided version. |
| `determinism.cross_leg` | **M11.a.** The digest one continuous-integration leg publishes for another leg to compare, and the five checks that make it worth comparing. It is the publisher half of the one job three criteria look for — `m9:lockstep-cross-platform`, `m10:pcg-regeneration-cross-platform` and `m10:pcg-gpu-domain-agreement` — and it computes its numbers out of the engine's own pieces: the golden session above, and the forest graph `src/pcg/tests/test_determinism.cpp` already compares between two runs on ONE host. |

## The cross-leg comparison, which is the one thing a single leg cannot do

Every other suite in this directory asks whether the engine agrees with its own committed
expectations. None of them asks whether **two architectures agree with each other**, and neither did
any job in `ci.yml`: its platform legs ran independently. That is why `m9:lockstep-cross-platform` was a
**declared gap** rather than a `where = "ci"` criterion — `where = "ci"` would have made it PASS in
continuous integration, satisfied by a single-leg suite, which is the defect the practice exists to
catch.

```
just test-determinism --publish-digest cross-leg-digests/<leg>.digest   # one leg, in CI
just test-determinism --compare-legs --pcg --digests cross-leg-digests  # the comparison
```

The comparator is `tools/ci/cross_leg_digests.py` and **what it refuses is its whole value**: one
leg, several legs of one architecture, a leg that could not name its own architecture, a digest that
is zero, a digest over an empty workload, and a schema it does not understand each exit **2** —
refused, never passed. A disagreement exits **1** with every leg's number printed beside its
architecture, because a disagreement is a finding rather than a failure to hide. Fifteen negative
fixtures hold that in `tools/ci/test_cross_leg_digests.py`, run by `just ci-check`.

The architecture compared is the one the **binary** detected, never the workflow's label for the
leg, so two runners of one architecture cannot be made into two by naming them differently.

The nightly `milestone` job downloads the same publisher artifacts before running
`just roadmap-milestone m11b --ci`. Without that download, its two M11.a cross-architecture
criteria would fail for missing digests even when the separate comparison job succeeded.
`tools/ci/test_cross_leg_ledger.py` checks the dependency, artifact routing, the condition that
still runs the ledger when one publisher fails, and the `--ci` invocation with mutations that must
fail. The publisher uploads its digest even if its test reports a divergence, so the comparator can
show the differing value. The real Linux arm64 and x86-64 CI digests for run
[`35843497860`](https://github.com/CyberdyneCorp/CyberdyneEngine/actions/runs/35843497860)
agree on both lockstep fields and both PCG fields; the comparison still runs in CI on every push.

**What this does not answer**, said here rather than left to be assumed: `cy::pcg::ExecutionDomain`
is Editor, Cook, Runtime, Streaming and Dynamic — there is **no GPU execution domain in this tree**
— and no hosted runner has a device, so `m10:pcg-gpu-domain-agreement` stays open. The last case in
`test_cross_leg.cpp` asserts that no domain names a device, and goes red the day one is added.

## What is declared from elsewhere, and runs in this kind

A module's suites are declared from the module in this tree, and these two were written at M6:

| Suite | Declared by |
|---|---|
| `determinism.save_fuzz` | `src/save/tests/CMakeLists.txt` — the exhaustive truncation and corruption sweeps over a save chunk. `testing-and-quality` lists "replay and save fuzzing" in this row. |
| `determinism.save_kill_nine` | `src/save/tests/CMakeLists.txt` — a child process SIGKILLed at each phase of a real commit, with the previous generation still loadable. That is "transactional save tests: failure injected after each write phase" in its strongest form. |

Both moved from `integration` to `determinism` at M10 rather than being copied here. The in-process
half of the transactional claim stays in `src/save/tests/test_archive.cpp`, beside the archive model
it is a property of.

## Regenerating the golden artefacts, deliberately

```
CY_DETERMINISM_RECORD_GOLDEN=1 ctest --test-dir <build> -R determinism.golden_replay
```

rewrites both files **and fails the run**, so a regeneration cannot happen by accident inside a green
CI job. The diff is then reviewed: one hash per tick, so it names the first tick at which the
simulation moved rather than reporting that a number changed. That is the whole of
`testing-and-quality`'s "so a regression and a deliberate behaviour change are distinguishable".

## Still owed here

`testing-and-quality`'s list for this suite is longer than what is declared above, and the rest lives
in `unit`/`integration` today rather than nowhere:

- the same simulation run twice to identical per-tick hashes — `integration.replay_session`
- identical results across worker counts and under chaos scheduling — `integration.determinism_scale`,
  which models a scheduler through `ExecutionConditions::order_seed` and is single-threaded; nothing
  runs one simulation under 1, 8 and 16 real workers, and `m9` says so
- re-simulation during network reconciliation — `integration.networking_session`
- hierarchical hashing that narrows a divergence to a field — `unit.determinism`

`cy::test::SeededRandom` in `tests/harness/` is here early on purpose: its sequence is pinned by a
unit test, because a generator that is only reproducible within one build is no use to a test that
compares two platforms.

**Governed by**: `testing-and-quality`, `simulation-and-determinism`.
