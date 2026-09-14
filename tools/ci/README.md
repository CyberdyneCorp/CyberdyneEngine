# `tools/ci/`

Layer 7. The checks that guard the **developer workflow and its continuous integration**, as
distinct from the checks that guard the engine. Nothing here is linked into anything; each is a
script a recipe invokes.

| File | Task | Checks |
|---|---|---|
| `check_workflows.py` | 2.4.4 | Every command in `.github/workflows/` is a `just` recipe or a tool install, names no build or quality tool directly, and invokes only recipes that exist. `--selftest` runs it over deliberately bad steps; `--list` prints the gate set. |
| `test_env_doctor.py` | 2.2.4 | `just env-doctor` against deliberately broken environments: a missing tool, a too-old tool, no compiler, and several problems at once. Asserts the exit status, the report line, and that every failure is followed by the correction for this host. |
| `test_recipes.py` | — | Invariants a gate downstream of a recipe depends on: a sanitized build tree is never the ordinary one, a recipe that parses flags binds them to `$@`, and the editor is built into the tree the override names. |
| `cross_leg_digests.py` | M11.a §4 | **Not a check of the workflow but a step inside one.** It compares the state digests the matrix legs published — the simulation hash and the generated world's — and it is the second half of the one job `m9:lockstep-cross-platform`, `m10:pcg-regeneration-cross-platform` and `m10:pcg-gpu-domain-agreement` all look for. `determinism.cross_leg` publishes; this compares. Invoked as `just test-determinism --compare-legs [--pcg]`. |
| `test_cross_leg_digests.py` | M11.a §4 | Fifteen negative fixtures for the comparator above, because **what it refuses is its whole value**: one leg, several legs of one architecture, a leg that cannot name its architecture, a zero digest, an empty workload, an unreadable schema — each exits 2, refused rather than passed. `m9:lockstep-cross-platform` was declared a gap rather than a `where = "ci"` criterion precisely because a single-leg suite would have satisfied the latter. |

All five run from `just ci-check`, which the `quality` job invokes — except `cross_leg_digests.py`,
which `ci-check` reaches only through its selftest, because the comparator itself needs digests that
only a multi-leg run produces.

**Why they are here rather than in `tests/`.** `testing-and-quality`'s taxonomy is about the engine:
unit tests are sub-millisecond and link engine code, and these link nothing and spawn processes.
They are tooling that checks tooling, which is what `tools/` is for.

**Why they are separate scripts and not a recipe's inline shell.** A negative fixture needs a
sandboxed `PATH` and a temporary directory per case. That is a program, and a program in a justfile
is a program nobody can run under a debugger.

**Governed by**: `developer-workflow-and-just`, whose "Forbidden workflow patterns" requirement says
each pattern it forbids shall be checkable. Two of them are checked here:

- *a continuous integration script that duplicates rather than invokes recipes* — `check_workflows.py`
- *a required developer task documented only as prose with no recipe* — partly: `check_workflows.py`
  fails a workflow step that is a raw command, which is where that pattern appears in practice.
