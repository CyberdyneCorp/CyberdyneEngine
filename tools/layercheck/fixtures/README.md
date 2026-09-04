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
| `gpuapi-above-backends/` | `layercheck.py --check gpuapi` | fail: a file above `src/backends/` includes `<vulkan/vulkan.h>` |
| `barrier-outside-graph/` | `layercheck.py --check barriers` | fail: a render pass emits its own barrier instead of declaring a resource use |
| `bare-target/` | `layercheck.py --check targets` | fail: a bare `add_library()` in the engine tree |
| `legal/` | all of the above | pass, and configure cleanly |

The fixtures are configured, never built: the check under test is a configure-time one, and the
sources exist only because CMake requires a target to have some.

`barrier-outside-graph/` is M3 task 2.2.4's "prove it by introducing one". Introducing a violation
by hand proves the gate fired on the day somebody ran it; keeping the violation here proves it on
every pull request instead, which is the property the invariant actually needs — barriers being
computed rather than written is a property of the thirtieth pass, and the thirtieth pass obeys it
because the first one did.

`layercheck.py` excludes this directory from its own scan of the repository, since every file here
is a deliberate violation.
