# Build the editor before the sessions that drive it, not inside them

## Why

Three CI jobs were red on `main` for one reason: the Rust editor is Cargo's, no CMake target builds
it, and the CTest entries that drive it built it themselves, inside their own timeout.

- **`agent`** failed on every run. `smoke.agent_authoring` passed `--build`, and on a hosted runner
  with no cached editor the cold build of the workspace was still compiling `cy-editor-testhost` when
  the 300 s smoke timeout expired. The session never started.
- **`authorable`** failed intermittently the same way: `smoke.editor_session` spent its budget on the
  build whenever the cache missed.
- **`profiles`** (and every `test` leg) failed `smoke.authoring` with "cyberdyne-editor was not
  built". That entry builds nothing, and its driver looked for the editor in
  `<build-dir>/editor/<profile>/` with a hand-copied profile mapping; `just build-editor` writes to
  `just _editor-target-dir` (`build/editor/` by default) in the directory `_cargo-profile` names, so
  under an ordinary `build/<profile>` tree it looked where nothing was built, and in `profile` and
  `release` it named Cargo directories that do not exist and fell back to whichever profile's editor
  a glob sorted first. `just run-authoring` had the same defect.

The smoke budget was never the problem; the build being inside it was. `docs/roadmap/capability-matrix.md`
recorded this as finding 6 at M5.5 ("marginal rather than safe").

## What Changes

- `samples/editor_fixture.cmake` declares `smoke.editor_build`, which runs `just build-editor` for the
  tree's profile as the setup of the `cy_editor` CTest fixture, with a timeout of its own
  (`CY_EDITOR_BUILD_TIMEOUT`, 1800 s). CTest runs a required setup first and adds it to any selection
  that needs it, so `just test-smoke -R agent_authoring` still builds the editor, outside the
  session's budget.
- `smoke.editor_session`, `integration.editor_session_selftest`, `smoke.agent_authoring`,
  `smoke.authoring`, `smoke.editor_window` and `smoke.editor_window_mcp` require `cy_editor` and no
  longer pass `--build`. Their timeouts are unchanged.
- `samples/08a-authoring/authoring.py` finds the editor through `just _editor-binary`, the recipe that
  owns the path, like every other editor driver.
- Regression tests: `integration.editor_fixture` (`tools/ci/editor_fixture.py`) reads the configured
  tree's test list and fails when an editor driver does not require the fixture or passes `--build`,
  with negative cases of its own; `tools/ci/test_recipes.py` gains "the authoring driver finds the
  editor the recipe built" across all four profiles and three `CY_BUILD_DIR` shapes.

## Non-goals

- Raising any test's timeout or budget.
- Caching the editor in more CI jobs. The fixture's timeout covers a cold build; caching is an
  optimisation for later.
- The other failures in the `profiles` and `test` legs (`integration.pcg_gpu_vulkan` on a runner with
  no Vulkan device, `integration.build_lightmap_cli` timing out, `smoke.shader_targets` over its CPU
  budget in Debug). They are not editor builds and are outside this change.

## Impact

- `testing-and-quality`: the test taxonomy states that building what a test drives is a fixture with
  its own bound, not part of the test's budget.
- CI: `agent` and `authorable` run their sessions against a built editor; `profiles` and `test` build
  the editor once per tree before `smoke.authoring`.
