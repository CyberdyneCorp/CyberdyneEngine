# Tasks

## 1. Measure

- [x] 1.1 Time the Windows and Linux `test` jobs on recent `main` and PR runs by step, and record
  cache hits and misses and the Ninja step counts inside `just test-all`.
- [x] 1.2 Find out why an exact hit still rebuilds everything: the checkout gives sources a later
  time than the restored objects.

## 2. Hand-off

- [x] 2.1 Add `just ci-build-tree-pack` and `just ci-build-tree-unpack`, which record the commit,
  refuse another commit or modified sources, and backdate tracked files.
- [x] 2.2 Add `test_recipes.py` cases for the round trip, refusal at another commit, refusal over a
  modified source, and refusal to pack a dirty tree. Each case is shown red by a mutation of the
  recipe.
- [x] 2.3 Make the `build` legs upload `build-tree-<label>`, and make `test`, `world`, `render`,
  `playable`, `authorable`, `scale` and `agent` unpack it after their editor builds.

## 3. Cache budget

- [x] 3.1 Remove the commit from the build-tree and editor cache keys.
- [x] 3.2 Make `cross-leg-publish`, `quality`, `generated` and `identity` restore without saving.

## 4. Guard

- [x] 4.1 Add a `check_workflows.py` rule for the hand-off, with six negative fixtures and one legal
  fixture. Each rule branch is shown red by a mutation, and the rule rejects `main`'s workflow.
- [x] 4.2 Update `docs/guides/building.md` and `tools/ci/README.md`.

