# `tests/determinism/`

Reproducibility of simulation and replication: the same inputs producing the same state, hashed per
tick. Budget: 10 s per test.

**Empty until M9**, the integrity milestone that lands determinism and networking. Determinism is a
property of a simulation, and there is no simulation to be deterministic about before then — a test
here now could only assert that nothing is nothing.

What M9 wires here, from `testing-and-quality`:

- the same simulation run twice produces identical state hashes per tick
- identical results across different worker counts and under chaos scheduling, since both expose
  undeclared ordering dependencies
- re-simulation during network reconciliation reproduces the original result
- hierarchical state hashing that narrows a divergence to the entity, component and field
- golden replays: recorded sessions with committed hashes, replayed in CI, so a regression and a
  deliberate behaviour change are distinguishable
- replay and save fuzzing, where a malformed save fails diagnostically and never crashes
- transactional save tests: failure injected after each write phase, with the previous save still
  valid

`cy::test::SeededRandom` in `tests/harness/` is here early on purpose: its sequence is pinned by a
unit test, because a generator that is only reproducible within one build is no use to a test that
compares two platforms.

**Governed by**: `testing-and-quality`, `simulation-and-determinism`.
