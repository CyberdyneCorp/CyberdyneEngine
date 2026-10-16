# Tasks

## 1. Measure

- [x] 1.1 Read the last unsharded nightly (run 38687291984): 134 of 484 criteria in six hours, the
  per-criterion seconds, and the failures that were the runner's rather than the tree's.

## 2. Shards

- [x] 2.1 `--shard i/n` and `--shard-record` on `roadmap.py milestone`; the assignment keeps
  tree-sharing criteria together and balances on measured seconds.
- [x] 2.2 `roadmap.py merge` / `just roadmap-merge`, refusing missing, duplicated, foreign and
  incomplete records; `--estimate` and `--write-costs`.
- [x] 2.3 `shard-platforms.toml` and the macOS, Windows and lavapipe shards.
- [x] 2.4 `test_ledger_shards` in `tools/roadmap/selftest.py`, each refusal shown red by a mutation of
  `shard.py`.

## 3. Workflow

- [x] 3.1 Twelve Linux shards, the platform shards, and `milestone-verdict` under `always()`.
- [x] 3.2 `check_workflows.py`'s shard rule with negative fixtures.
- [x] 3.3 The tools the criteria run, installed in the shards.

## 4. What the shards exposed

- [x] 4.1 RHI default fallback past a backend that cannot create a device (`unit.rhi`).
- [x] 4.2 `samples/10-world` budget CSV columns (`smoke.world_budget_csv`).
- [x] 4.3 The editor crash tests build the runtime stub they kill.
- [x] 4.4 The editor transport enables only extensions the device offers.
- [x] 4.5 `test_animation_service.cpp` under `CY_ANIMATION=OFF`.
- [x] 4.6 `falsify` proves each distinct check once.
- [x] 4.7 `quality-lint`'s per-commit pass cache, with `test_tidy_cache.py`.
