# `tools/roadmap/` — layer 7

The roadmap tooling: what is implemented today, what closes the current milestone, and the gate set
a change has to pass. Three recipes, four data files, and no judgement anywhere in between.

```
just roadmap-status                # every capability's tier, milestone and change
just roadmap-milestone m0          # M0's exit criteria, run
just roadmap-milestone m2 --list   # what M2's are, without running them
just roadmap-gates                 # the permanent merge gates and any recorded override
just roadmap-test                  # the tooling's own tests, including the three drift cases
```

## What each part is

| Path | Is |
|---|---|
| `docs/roadmap/status.yaml` | The record. One entry per capability: tier, the milestone that last advanced it, the change that did so. Not owned by this directory — owned by whoever advances a capability. |
| `tools/roadmap/milestones/<id>.toml` | One milestone's exit criteria. `m0.toml`, `m1.toml` and `m2.toml` today; M3 through M11 add a file each and change no code. |
| `tools/roadmap/gates.toml` | The permanent merge-gate set, and the overrides recorded against it. |
| `record.py`, `criteria.py`, `gates.py` | Reading and validating those three. Each raises one error type with a message that names the file, the line or the entry, and what to do. |
| `roadmap.py` | The command line behind the recipes. |
| `selftest.py` | The tests. `just roadmap-test`. |

Everything is standard-library Python: these run on every pull request, on three platforms, and a
gate may not depend on a package that happens to be installed. `status.yaml` is read by a parser in
`record.py` rather than by a YAML library for that reason — the file is a deliberately restricted
shape, and anything outside it is an error with a line number.

## Status, and why it fails

`delivery-roadmap` requires exactly one authoritative record of implementation status, and requires
the recipe that reports it to **fail** when the record and `openspec/specs/` disagree. So
`roadmap-status` exits non-zero when:

- a capability has a specification and no entry — it was added and the record was not updated
- an entry names a capability with no specification — it was renamed or removed
- a tier above `none` names no milestone or no change — a claim nobody can trace
- a tier, a milestone id, or the file's shape is not one this tool recognises

`just roadmap-test` proves the first three by constructing them, against a temporary copy of the
record: adding a fake capability, deleting an entry, renaming one. A gate whose failure path is
never exercised is a gate that has quietly stopped firing.

## Milestone criteria are data

`delivery-roadmap` requires exit criteria to be executable checks and every milestone to be closable
by one recipe. The criteria are therefore data — one TOML file per milestone — and `criteria.py` is
the only thing that knows how to run them. Four kinds, which is all the specification's definition
of a criterion allows:

| `kind` | Passes when |
|---|---|
| `recipe` | a `just` recipe exits zero |
| `command` | a shell command exits zero |
| `path` | a committed artefact exists at a path |
| `tiers` | the status record carries the tiers this milestone exits at |

Every criterion carries `source` — the line in `tasks.md` or `ROADMAP.md` it comes from — and
`ci_job`, the gate in `gates.toml` under which continuous integration runs it. Both are required.
A criterion that passes on one laptop and runs nowhere else is not a gate, and the loader refuses
one that names a job no gate declares.

**Nothing is silently skipped.** A criterion this host cannot evaluate — another operating system
(`where = "ci"`), no window system (`requires = "display"`) — must carry a `reason`. It is then
reported as *not evaluated*, never as passed, is counted separately in the summary, and names the CI
job that does evaluate it. `--ci` runs those criteria too, for the platform-specific jobs.

Exit status: `0` every criterion this host evaluated passed, `1` one failed, `2` the data is wrong.

## Gates, and overrides

`gates.toml` is the permanent set `testing-and-quality` requires: the three-platform build and test,
format, lint, layering, generated-code currency, spec validation, and the status record. M1 added
seven more — type and field identity, the reflection round-trip goldens, the project graph's
rejections, the sanitizers over the job suite, the job system's throughput benchmark, the three
non-default profiles, and the workflow check itself. M2 added four, and each is a property a change
can break without breaking a test that names it: the state hash reproducing across processes and
across a snapshot restore, an authored hierarchy lowering to archetype blocks, the coherence
invariants between a node and the entity it is a handle onto, and structural change being observable
only at flush points. It exists as data so that continuous integration, the contributor
documentation and this tool name the same gates — three hand-maintained copies of a list diverge,
and the copy that drifts is the one in CI.

`tools/ci/check_workflows.py` is the check that stops the CI copy from drifting: a gate declared
`class = "permanent"` whose commands no job in `.github/workflows/` runs fails `just ci-check`. So a
gate is declared and its job lands in the same change, or neither does.

`just roadmap-gates --commands` prints exactly what a workflow must run, one command per line.

A failing gate is fixed. Where it genuinely cannot be, `testing-and-quality` requires an explicit
recorded override rather than a quiet exception, so an override is an `[[override]]` table naming
the gate, the reason, the approver, the change that records it, and an expiry. Every field is
required and an expired override **fails** this check: the override that outlives its reason is the
quiet exception under another name. There are none today.

## Milestone gates do not regress

A milestone's criteria join the permanent set when the milestone closes and stay green afterwards —
`delivery-roadmap`, Milestone gates do not regress, and `testing-and-quality`, Quality gates for
merge. That is recorded in `gates.toml` as a gate of `class = "milestone"`, carrying
`state = "joins-on-close"` until the milestone closes and `state = "green"` from then on. From that
point a change that breaks `just roadmap-milestone m0` does not merge unless it lands the
criterion's recorded replacement in the same change.

M1 is the first milestone to have to obey that rule, and it did not: two of its modules landed with
findings against the `lint` gate M0 closed with, which turned `just roadmap-milestone m0` red. So
`m1.toml`'s first criterion runs `just roadmap-milestone m0` — the rule as something the closing
recipe executes rather than something a reviewer is expected to remember. Every milestone ledger
after this one carries the same criterion for the milestone before it, `m2.toml` included, and
`selftest.py` checks each rung rather than trusting the next author to remember: the omission is
invisible until the day it matters, which is the day somebody closes a milestone on a broken one.

`just roadmap-test` checks the ladder itself on every pull request: every ledger under
`milestones/` loads, every criterion in it names a gate that exists, every milestone with a ledger
has a `class = "milestone"` gate for its criteria to join, and each rung runs the one below it. The
milestone recipes each take a working session, so a ledger that has stopped loading has to fail
somewhere cheaper than the day somebody tries to close a milestone.

**Governed by**: `delivery-roadmap`, `testing-and-quality` (Quality gates for merge).
