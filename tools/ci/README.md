# `tools/ci/`

Layer 7. The checks that guard the **developer workflow and its continuous integration**, as
distinct from the checks that guard the engine. Nothing here is linked into anything; each is a
script a recipe invokes.

| File | Task | Checks |
|---|---|---|
| `check_workflows.py` | 2.4.4 | Every command in `.github/workflows/` is a `just` recipe or a tool install, names no build or quality tool directly, and invokes only recipes that exist. `--selftest` runs it over deliberately bad steps; `--list` prints the gate set. |
| `test_env_doctor.py` | 2.2.4 | `just env-doctor` against deliberately broken environments: a missing tool, a too-old tool, no compiler, and several problems at once. Asserts the exit status, the report line, and that every failure is followed by the correction for this host. It also verifies that the native CI runner architecture wins over the architecture reported by an emulated shell, which is required for Windows ARM64 tool exceptions. |
| `test_recipes.py` | — | Invariants a gate downstream of a recipe depends on: a sanitized build tree is never the ordinary one, a recipe that parses flags binds them to `$@`, the editor is built into the tree the override names, and the machine-wide job pool holds — overlapping builds never exceed it, a link gets no jobserver and holds the slots its `-flto=N` names, a real GCC LTO link started inside a saturated pool completes, and LTO links overlapping compiles never exceed the pool. |
| `cross_leg_digests.py` | M11.a §4 | **Not a check of the workflow but a step inside one.** It compares the state digests the matrix legs published — the simulation hash and the generated world's — and it is the second half of the one job `m9:lockstep-cross-platform`, `m10:pcg-regeneration-cross-platform` and `m10:pcg-gpu-domain-agreement` all look for. `determinism.cross_leg` publishes; this compares. Invoked as `just test-determinism --compare-legs [--pcg]`. |
| `cross_leg_audit.py` | M11.a §4, repair 1 | **The three criteria that job answers, made behavioural.** `m9:lockstep-cross-platform`, `m10:pcg-regeneration-cross-platform` and `m11a:cross-leg-digest-job` were searches for words in a job block — `download-artifact`, `digest`, `lockstep`, `pcg` — and M11.a's gate refuted all three at once: a six-line dummy job that moves no bytes and runs `echo` turns every one of them green. This reads the workflow only far enough to find the publisher, the downloader and the command between them, and then RUNS that command against digests it wrote: agreeing legs must pass, and a changed hash, a changed world, one leg, one architecture, a zero digest and an empty workload must each turn it red. **And a command is not a job**, which the repair-2 gate proved by breaking the workflow three ways — `if: false` on the comparison job (a skipped job is scored as SUCCESS, so GitHub stays green too), the leg's publishing step deleted, that step's command replaced by an `echo` — and watching all three criteria stay green. So it also evaluates every `if:` on the two jobs, the `needs:` chain and each step of the chain against the workflow's own `on:` triggers (a condition it cannot read is a finding, never a pass), and RUNS the leg's own publishing command with its output redirected and the build made impossible, to see whether that command routes the path its job uploads. `--selftest` runs it over twelve workflows it must refuse — the dummy first, then the gate's own three — and requires each refusal to SAY the thing its fixture is about, so a rule that began refusing everything cannot pass for twelve discriminating ones. |
| `test_cross_leg_digests.py` | M11.a §4 | Fifteen negative fixtures for the comparator above, because **what it refuses is its whole value**: one leg, several legs of one architecture, a leg that cannot name its architecture, a zero digest, an empty workload, an unreadable schema — each exits 2, refused rather than passed. `m9:lockstep-cross-platform` was declared a gap rather than a `where = "ci"` criterion precisely because a single-leg suite would have satisfied the latter. |

All six run from `just ci-check`, which the `quality` job invokes — except `cross_leg_digests.py`,
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
