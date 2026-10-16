# Split the nightly ledger into shards merged into one verdict

## Why

The nightly `milestone` job ran all 484 closed-milestone criteria in one hosted job and never
passed. GitHub cancels a hosted job at six hours. Run 38687291984 had reached 134 criteria when it
was cancelled, and `m1:four-profiles` alone took 7680 s there. Some criteria also failed only because
the runner lacked a tool: the libclang Python bindings, Pillow, matplotlib, or a Vulkan device. And
`m0:lint` needed 44 minutes on four cores against an 1800 s budget. Two build trees filled the disk
because every tree of the run shared one runner.

## What Changes

- `roadmap.py milestone <id> --shard i/n --shard-record FILE` evaluates one shard of the same flat
  plan and writes its results. Criteria sharing a build tree other than the default one share a
  shard. Shards are balanced on the seconds `tools/roadmap/shard-costs.toml` measured on the hosted
  runner, with an estimate for an unmeasured criterion based on what it holds.
- `roadmap.py merge` (`just roadmap-merge`) recomputes the assignment. It refuses a record that is
  missing, duplicated, from another commit, count or `--ci` setting, or that evaluated a set other
  than its shard's, then summarises the whole plan and prints each shard's duration. A missing shard
  fails the verdict.
- `tools/roadmap/shard-platforms.toml` moves criteria a plain Linux shard cannot answer to a runner
  that can. Metal goes to macOS and D3D12 to Windows; only a `where = "ci"` criterion may move to
  another system. The two criteria that refuse a skipped device suite as evidence go to a Linux
  runner with lavapipe.
- `.github/workflows/ci.yml` runs twelve Linux shards, the platform shards, and `milestone-verdict`
  under `always()`. `tools/ci/check_workflows.py` guards that shape with negative fixtures.
- `just quality-lint` with `CY_TIDY_CACHE` reuses the `quality` job's per-file passes at the same
  clean commit.
- `falsify` proves each distinct check once per run, so `just roadmap-test` fits
  `m7:plan-consistency`'s 1800 s on a hosted runner.
- The shards also exposed five defects, each fixed with a regression test:
  - the RHI default chain did not fall back past a backend that fails to create a device;
  - the world sample's budget CSV had blank `_ms` cells;
  - the editor crash tests depended on a stub only an earlier build had left;
  - the editor's transport named device extensions the device did not offer;
  - a test file broke the `CY_ANIMATION=OFF` build.

## Impact

- Affected specs: `developer-workflow-and-just`.
- Affected code: `tools/roadmap/{shard,roadmap,falsify,selftest}.py`, `tools/roadmap/shard-*.toml`,
  `tools/ci/check_workflows.py`, `tools/ci/test_cross_leg_ledger.py`, `tools/quality/tidy_cache.py`,
  `just/{roadmap,quality,ci}.just`, `.github/workflows/ci.yml`, `src/backends/rhi/src/backend.cpp`,
  `samples/10-world/main.cpp`, `src/editor_backend/tests/test_animation_service.cpp`,
  `editor/crates/cy-editor-app/tests/survives_a_runtime_crash.rs`,
  `editor/crates/cy-editor-viewport-transport/src/device.rs`.
