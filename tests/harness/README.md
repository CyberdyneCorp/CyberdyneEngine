# `tests/harness/`

The framework seam and the fixtures. Every test binary links `cy::test-harness`, and this is the
only directory permitted to name doctest.

| File | What it is |
|---|---|
| `include/cy/test/test.h` | `CY_TEST_CASE`, the assertions, and the budget guard. The one include a test needs. |
| `include/cy/test/fixtures.h` | The injectable fixtures: a deterministic clock, a seeded generator, a temporary directory. |
| `src/main.cpp` | doctest's `main`, so no test file carries one. |
| `src/budget.cpp` | The per-test budget check. |
| `src/fixtures.cpp` | The filesystem half of the fixtures. |

## Why a wrapper

`design.md` §5 chose doctest on a compile-time argument — an argument about today's numbers at
today's scale. The wrapper is what keeps that choice reversible: replacing the framework is a change
to this directory, not to every test in the tree. The seam is enforced twice, at configure time and
at run time, because a seam nobody checks is a seam that has already been crossed.

## What the harness does not have yet

`testing-and-quality` names a fuller set than this: scene and world fixtures, a mock platform and
display server, an in-memory filesystem mount, network condition simulation, image comparison, and
state hashing. Each is a mock or a comparison of an interface that does not exist yet, and a mock
written before its interface is a guess that has to be rewritten.

| Fixture | Arrives with |
|---|---|
| Mock `Platform` and `DisplayServer` | M0's headless implementations, once `core-platform-abstraction` has landed |
| World and scene fixtures | M2, with the ECS and the scene graph |
| In-memory filesystem mount | M2, with the asset pipeline's virtual filesystem |
| Image comparison | M3, with the renderer and `tests/render/` |
| State hashing, network conditions | M9, with determinism and replication |

**Governed by**: `testing-and-quality`.
