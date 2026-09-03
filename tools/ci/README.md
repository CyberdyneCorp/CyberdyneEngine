# `tools/ci/`

Layer 7. The checks that guard the **developer workflow and its continuous integration**, as
distinct from the checks that guard the engine. Nothing here is linked into anything; each is a
script a recipe invokes.

| File | Task | Checks |
|---|---|---|
| `check_workflows.py` | 2.4.4 | Every command in `.github/workflows/` is a `just` recipe or a tool install, names no build or quality tool directly, and invokes only recipes that exist. `--selftest` runs it over deliberately bad steps; `--list` prints the gate set. |
| `test_env_doctor.py` | 2.2.4 | `just env-doctor` against deliberately broken environments: a missing tool, a too-old tool, no compiler, and several problems at once. Asserts the exit status, the report line, and that every failure is followed by the correction for this host. |

Both run from `just ci-check`, which the `quality` job invokes.

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
