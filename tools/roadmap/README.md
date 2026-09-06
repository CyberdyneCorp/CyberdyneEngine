# `tools/roadmap/` — layer 7

The roadmap tooling: what is implemented today, what closes the current milestone, and the gate set
a change has to pass. Three recipes, four data files, and no judgement anywhere in between.

```
just roadmap-status                # every capability's tier, milestone and change
just roadmap-milestone m0          # M0's exit criteria, run
just roadmap-milestone m3 --list   # what M3's are, without running them
just roadmap-gates                 # the permanent merge gates and any recorded override
just roadmap-test                  # the tooling's own tests, including the three drift cases
```

## What each part is

| Path | Is |
|---|---|
| `docs/roadmap/status.yaml` | The record. One entry per capability: tier, the milestone that last advanced it, the change that did so. Not owned by this directory — owned by whoever advances a capability. |
| `tools/roadmap/milestones/<id>.toml` | One milestone's **new** exit criteria — what it adds to the permanent set, not what it inherits. `m0.toml` through `m4.toml` today; M5 through M11 add a file each and should change no code — M3 added one line, the `gpu` requirement below, because it is the first milestone whose criteria need hardware, M4 added the `MINIMUM_CRITERIA` floors below, because its ledger was otherwise covered by nothing, and M5 removed the `m<n>-green` chaining criteria for the reason under "A ledger is flat". |
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
(`where = "ci"`), no window system (`requires = "display"`), no graphics device
(`requires = "gpu"`) — must carry a `reason`. It is then reported as *not evaluated*, never as
passed, is counted separately in the summary, and names the CI job that does evaluate it. `--ci`
runs those criteria too, for the platform-specific jobs.

`gpu` joined `display` at M3, which is the first milestone with criteria that need hardware: the
conventions sampled back off a device, the golden images, and a frame run with the validation layers
on. The probe is the presence of a DRM render node, with `CY_HAS_GPU` as the override for the cases
a file cannot answer. It is deliberately not "run `vulkaninfo`" — this module runs on every pull
request on three platforms, and a gate that shells out to a tool that may not be installed is a gate
that fails for the wrong reason. The failure this guards against is the expensive one: a milestone
recipe that quietly skipped its rendering criteria would report M3 green on exactly the machines
least able to judge it.

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
only at flush points. M3 added four: the graph deriving barriers, aliasing and scheduling from
declarations alone and producing an identical plan twice; the renderer above it — the RHI, the
render server, the pass order, the BRDF, the lights and the culling — all of which runs with no GPU;
the shader pipeline over the SPIR-V passthrough, with the Slang front end off; and the render suites
themselves, whose gate says out loud which of them a machine without a device actually runs. What
M3 deliberately did NOT declare is a golden-image gate: a committed reference is a photograph of one
implementation and a hosted runner is another, so the golden images are a criterion carrying
`requires = "gpu"` rather than a gate nobody can run. It exists as data so that continuous
integration, the contributor documentation and this tool name the same gates — three hand-maintained copies of a list diverge,
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
findings against the `lint` gate M0 closed with, which turned `just roadmap-milestone m0` red. So the
rule became something the closing recipe executes rather than something a reviewer is expected to
remember. **How it executes changed at M5**, and the section below is why.

## A ledger is flat, and every distinct criterion runs once

`just roadmap-milestone <id>` evaluates two things: the **permanent set** — the criteria of every
milestone whose gate in `gates.toml` is `state = "green"` and which sits below this one on the
ladder — and the milestone's **own** criteria. The two are merged, the declarations that do the same
work are collapsed, and each distinct check runs exactly once. `criteria.build_plan` is that rule;
`gates.permanent_milestones` is where it reads what has joined the set. **No ledger invokes another
ledger.**

It used to. Each ledger's first criterion ran the previous milestone's recipe — `m1.toml` ran
`just roadmap-milestone m0`, `m2.toml` ran M1's, and so on — so closing M4 ran a chain five deep.
Measured on this repository before the flattening, one run of `just roadmap-milestone m4` was:

| | chained | flat |
|---|---|---|
| criterion evaluations | 118 | **87** |
| distinct checks among them | 91 | 87 |
| redundant evaluations | 27 | **0** |
| `four-profiles` — a four-configuration build and test | **4×** | **1×** |
| profiled `build-engine` / `test-all` invocations | 16 / 16 | **4 / 4** |
| ledger invocations nested inside the run | 4 | **0** |
| wall clock, one workstation, warm trees | 70.9 min | **56.8 min** |
| wall clock if every repeat paid its first run's cost | 142.4 min | **56.8 min** |

Take the **failure count, not the clock, as the finding**. The flat run is 3,405 s measured; the
chained run is derived from the same per-criterion measurements, with the three repeats of
`four-profiles` priced at the 206 s a second run over already-built trees actually costs rather than
at the 1,637 s the first one did. That is a fifth of the time — worth having, not the point. The
point is the 27 redundant evaluations: 27 extra chances for something unrelated to the change under
test to go red, four of them on the one criterion this repository has measured flaking.

It compounds with the ladder, which is why M5 was the cheapest place to stop it — every rung below
pays for every rung above it:

| ledger | chained evaluations | flat |
|---|---|---|
| `m0` | 15 | 15 |
| `m1` | 35 | 29 |
| `m2` | 62 | 49 |
| `m3` | 91 | 69 |
| `m4` | 118 | **87** |

The chained column is quadratic in the length of the ladder and the flat column is the number of
distinct checks that exist. At M11 the chain would have been twelve deep.

The cost is not only time. Re-running one criterion n times multiplies its failure probability by n,
and a `unit.scene` case sitting on the taxonomy's per-case budget at `-O0` duly became a flake that
failed four ledgers at once — diagnosed as four problems before the multiplication was recognised as
the cause. Deduplication is a correctness property here, not an optimisation.

Two things had to survive the change, and each is silent when lost, so both are checks in
`selftest.py` (`test_flat_ledger`) rather than paragraphs:

- **Deduplication.** Each distinct check appears once, however many milestones declared it. The
  fingerprint is the *work* — the kind, the command or path or tier table, and the conditions it runs
  under — never the id: `sample-recipe` names a different sample in M2, M3 and M4, and collapsing
  those three by name would drop two milestones' closing artefacts. Where declarations differ only in
  `timeout_s`, the most generous budget wins; a budget is not a check.
- **The ladder.** An earlier milestone's criteria are still *in* the newest ledger, so a regression
  against M0 still fails it. Chaining provided this by construction; nothing but the check provides
  it now. A closed milestone's own ledger stays its own criteria — widening `milestone-m0`, a
  permanent merge gate, with everything a later milestone added would turn it red for work M0 never
  claimed.

Criteria merged from an earlier milestone are labelled by the milestone that declared them first, so
a failure reads `m0:layering`, once, rather than as a `m3-green` → `m2-green` → `m1-green` cascade
with the real failure buried in a truncated capture thirty lines down.

**The flip used to be the part that kept being forgotten, and it is now a check.** A milestone's
gate carries `state = "joins-on-close"` until it closes and `state = "green"` from then on, and three
times in a row the flip was done a milestone late: M0's and M1's were both flipped while M2 was
closing, and M2's while M3 was. The reminder in `gates.toml` demonstrably did not work — a comment
addressed to whoever reads the file next is not a check.

So `roadmap-test` now reads `openspec/changes/archive/` and fails when a milestone has a ledger, an
archived change, and a gate still at `joins-on-close`. The archive path is the fact a tool can read,
and M3's flip was the last one that depended on somebody remembering: **M3's own gate was already
`green` when M4 came to close**, which is the first time in four milestones that has been true. The
check does not fire for the milestone being closed right now, whose change is archived after its
recipe passes — so the closing change still sets its own state, and the check catches it on the very
next pull request if it did not.

`just roadmap-test` checks the ladder itself on every pull request: every ledger under
`milestones/` loads, every criterion in it names a gate that exists, every milestone with a ledger
has a `class = "milestone"` gate for its criteria to join, no ledger invokes another, and the newest
ledger evaluates every closed milestone's criteria exactly once. The milestone recipes each take a
working session, so a ledger that has stopped loading has to fail somewhere cheaper than the day
somebody tries to close a milestone.

It also checks that no ledger is a token gesture: `selftest.py`'s `MINIMUM_CRITERIA` records the
fewest criteria each milestone may carry, from its `ROADMAP.md` row and its section 6. That table
used to be one hand-written check per milestone, which made it one more thing the next author had to
extend — and it was not extended, so **M4's ledger landed covered by nothing**. The floors are now
data and the last check over them is the one that matters: a ledger under `milestones/` with no
floor recorded **fails**, rather than being quietly unchecked. Adding a milestone therefore forces a
deliberate answer to "how many exit conditions does this have", which is the question the floor asks.

**Governed by**: `delivery-roadmap`, `testing-and-quality` (Quality gates for merge).
