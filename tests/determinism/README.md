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
