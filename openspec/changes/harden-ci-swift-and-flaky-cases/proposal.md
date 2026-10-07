# Pin the CI Swift toolchain, and root-cause four intermittent failures

## Why

`main` went red without a code change in four places.

- **SwiftPM segfaults.** The `world`, `authorable`, `agent` and `sanitize` jobs built the Swift game
  modules with whatever Swift the runner image shipped, because they had no `setup-swift` step. The
  image moved to Swift 6.4, which made Swift Build SwiftPM's default build system, and its planner
  intermittently dies with signal 11 in libdispatch (`_dispatch_event_loop_drain`, right after
  swift-syntax resolves; swiftlang/swift-build#1786). Every crash in the runs examined was on 6.4
  (`authorable` in run 37548099658, `sanitize` in 37194285147 and 36943266067); no job pinned to
  6.0.3 crashed.
- **`smoke.quiet_host_marker` leg 6 (c)** (#80). The stall probe ran at
  `CY_TEST_BUDGET_SCALE=0.25`, a 0.25 ms CPU budget, and the vfork case's own thread is charged the
  kernel's fork: 0.02 to 0.3 ms on a busy 24-core host. The failures were `over budget:`, which the
  leg reported as "expected the vfork case to pass". Reproduced: 2 of 80 runs at 0.25 beside 24
  niced spinners, 0 of 80 at 1.
- **The editor session's `survive` act.** `cy-runtime-stub` printed `connected` at `accept`, and
  `session.py` kills it on that line. The editor sends its Hello after connecting and fails the
  attach when the session is already lost, so a kill between the two made the editor report the dead
  runtime on stderr before its script started and run the act with no runtime.
- **`unit.determinism` on macOS.** "the twelve replacements agree with <cmath>" went over its 1 ms
  budget at 1.12 to 1.38 ms on the hosted macOS runner. It is the first case to call most of libm,
  and the harness warmed only the test executable, so the case paid every first touch of libm's
  code. On Linux its CPU falls from a median of 0.042 ms to 0.021 ms once libm is warmed.

## What Changes

- `swift_pin_version := '6.0.3'` in the justfile. Every Linux x86_64 CI job that builds the tree
  installs it, and `tools/ci/check_workflows.py` fails a `setup-swift` step that installs another
  version, or a job that builds the tree without one.
- `bindings/swift/tools/cy_swift_module.py` passes `--build-system native` to `swift build` and
  `swift test`, and runs a Swift command again, at most twice, when a crash signal ended it, logging
  the reason. A compile error, SIGINT, SIGTERM or SIGKILL is never retried.
- The same driver stamps SwiftPM's scratch directory with the toolchain that wrote it and removes it
  when another builds there. Swift 6.0.3 refuses the `workspace-state.json` 6.4 writes ("unknown
  'WorkspaceStateStorage' version '7'"), and the jobs moving from the image's 6.4 to the pin restore
  build trees 6.4 wrote.
- The stall probe runs at budget scale 1 in `smoke.quiet_host_marker` and `integration.harness`. The
  300 ms holds remain three times the 100 ms ceiling, so every verdict is unchanged.
- `cy-runtime-stub` prints `connected` when the editor's Hello arrives.
- `warm_process_image()` also reads the C math library's code.
- Regression tests: `tools/ci/test_recipes.py` (crash classification, the retry bound, the native
  build system, another toolchain's state), `check_workflows.py --selftest` (the pin), `survives_a_runtime_crash.rs` (no
  `connected` before a Hello), and `unit.harness_image_warmup` (libm resident before the first case).

## Not fixed here

`m11a:world-budget-on-a-device` (#77) is root-caused but stays open. The frame grew from about 10.8
to 13 ms when `samples/10-world` gained water shading (#25, merge `6c66bda3`): its refraction and
reflection passes redraw the scene, and `--no-water-shading` on the same binary takes the
`stage_submit_ms` band from 7.0 back to 4.6 ms. The added cost does not change with resolution, so
it is geometry work rather than fill. Making it cheaper is a rendering change to the water passes,
not a CI change.

## Impact

CI workflow, the Swift module build driver, the test harness's start-up, the stall probe's budget
scale, and the editor's test runtime stub. No engine, ABI or shipped runtime code changes.
