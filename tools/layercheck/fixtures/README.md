# `tools/layercheck/fixtures/`

Task 1.3.4. Each fixture is a tree that the enforcement **must** reject, plus one it must accept.
`../selftest.py` runs them and asserts the outcome; an enforcement that has quietly stopped firing
shows up here as a passing fixture, which is a test failure.

| Fixture | Checked by | Must |
|---|---|---|
| `upward-link/` | `cmake/module.cmake` | fail configure: a layer 0 target links a layer 4 target |
| `upward-link-deferred/` | `cmake/module.cmake` | fail configure: the same, where the higher target is declared *after* the link that names it |
| `upward-include/` | `layercheck.py --check includes` | fail: a file under `src/core/` includes a header from `src/scene/` |
| `sdl-above-platform/` | `layercheck.py --check sdl` | fail: a file outside `platform/` includes `<SDL3/SDL.h>` |
| `bare-target/` | `layercheck.py --check targets` | fail: a bare `add_library()` in the engine tree |
| `legal/` | all of the above | pass, and configure cleanly |

The fixtures are configured, never built: the check under test is a configure-time one, and the
sources exist only because CMake requires a target to have some.

`layercheck.py` excludes this directory from its own scan of the repository, since every file here
is a deliberate violation.
