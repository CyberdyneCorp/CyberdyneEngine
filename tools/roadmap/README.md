# `tools/roadmap/` — layer 7

The roadmap tooling: what is implemented today, what closes the current milestone, and the gate set
a change has to pass. Three recipes, four data files, and no judgement anywhere in between.

```
just roadmap-status                # every capability's tier, milestone and change
just roadmap-milestone m0          # M0's exit criteria, run
just roadmap-milestone m3 --list   # what M3's are, without running them
just roadmap-milestone m11d --incremental --list   # which criteria a change can have moved, and why
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
| `falsify.py` | Whether a criterion can go RED. Breaks what each one names — in a sandboxed copy of the tree, or, for the criteria only a build can judge, in the working tree under `--mutate-the-tree` — and requires it to fail. `just roadmap-falsify`. |
| `falsifiability.toml` | Generated. What the ladder has shown can fail, and the list — which only shrinks — of what it has not. |
| `requirements.py` | Every requirement of a capability row against the test, gate or recorded exemption that answers it. `just quality-requirements <row>…`. |
| `requirements-coverage.toml` | Hand-written. The map `requirements.py` reads; every entry is resolved against the tree, so a renamed suite turns the row red. |
| `schedule.py` | What each criterion HOLDS while it runs — a build tree, Cargo, the device, a port — and the scheduler that therefore runs independent ones at the same time. A criterion whose needs it cannot read runs alone. |
| `matrix.py` | The build **matrix**: one tree per distinct build CONFIGURATION, under `build/ledger-matrix/`, shared by every criterion that needs it. Thirteen configurations between eight criteria, `m1:four-profiles` among them. Nothing is cached — it runs `just build-engine` for every row it is asked for, every time, and Ninja decides what is out of date. |
| `quiet_host.py` | Whether a criterion that runs through `just test-quiet-host` keeps its whole line inside it: nothing after the wrapped command but `\|\| exit <n>`, and `exclusive` declared. M11.c's eighth close found `m6:culling`'s second suite running after the wrapper had exited. Since the ninth close (option B) it no longer names suites that must never run bare: the harness itself enforces its stall ceiling only inside a verified `cy_quiet_host` and reports a stall anywhere else. `tools/quiet-host/README.md` has why. |
| `ledger_equivalence.py` | One ledger run sequentially and in parallel, compared verdict by verdict. Hours, not a pull-request gate. |
| `incremental.py` | Incremental closes: which criteria a change since the last green full ledger can have moved, read from the build graph, the recipes and the ledgers' digests. A criterion whose inputs cannot be read is always selected. `just roadmap-milestone <rung> --incremental`. |
| `incremental.toml` | Hand-written, reviewed and PINNED. Compile definitions that name the repository root and are only strings — `CY_DIAG_SOURCE_ROOT` — each with the files allowed to use it and a digest of their content, so the exemption lapses the moment the code it was reviewed over changes. |
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

## A criterion nobody has shown can fail is not a check

This repository has now found **seven** criteria that were green because nothing could make them
red: a determinism test whose scene never contended; a sky test asserting against the producer's own
statistics instead of the store; a criterion running through a recipe that passes
`--no-tests=ignore`; two whose grep matched only the ledger file doing the grepping; a dependency
check parsing backticks out of a table written in bold; and a four-profiles criterion that passed
only because the runner supplied a build step it lacked. Then M11.a and M11.b's gate refuted five
claims at once, three of them "word-greps a dummy job satisfies".

At seven it is not seven bugs. It is a defect in how a criterion is written, and six comments asking
for care have already failed to fix it. So the rule is enforced by tooling:

> **A criterion is not admitted to a ledger until the tooling has broken what it names and watched
> it go red.**

`just roadmap-falsify` is that tooling, and `just roadmap-test` — the `plan-consistency` criterion of
every ledger on the ladder — runs it on every pull request.

### What a proof is

Three runs against a sandbox that is a copy of the **tracked** tree, never the tree itself:

| Run | Must |
|---|---|
| positive control — unmutated | **pass**, or the criterion is reported *not provable here* with the reason, and is not counted as proven |
| the mutation | **fail** |
| ledger-blind — `tools/roadmap/milestones/` deleted | **pass** |

The third exists because two of the seven were a grep that found its own ledger. No regex catches
every spelling of that; deleting the ledgers catches all of them, because a criterion whose verdict
changes when the roadmap is deleted was reading the roadmap.

### A criterion that is already red has been watched going red

A proof therefore comes in five shapes. The second and third are for the criteria that cannot pass a
positive control because they are failing *right now* — which is most of a rung's ledger while the
rung is open — the fourth is for the ones whose subject is a **compiled artefact**, and the fifth is
for the ones this host cannot evaluate at all:

| Verdict | What was observed |
|---|---|
| `proven` | it passes, and the mutation the tooling applied turned it **red** |
| `red in the tree` | it **fails as written**, in the sandbox *and* in the repository |
| `red against a built tree` | the same, for a criterion a source-only sandbox cannot run at all, observed against a real build: `just roadmap-falsify prove --build-dir build/dev` |
| `proven against a built tree` | it **passes** against that build, the mutation applied to the **working tree** and rebuilt over turned it red, and restoring the tree turned it green again: `just roadmap-falsify prove --build-dir build/dev --mutate-the-tree` |
| `proven in the environment CI supplies` | a `where = "ci"` criterion: its `[criterion.ci_proof]` **built** the environment continuous integration hands it, the criterion passed against that, the mutation of *the environment* turned it red, and restoring it turned it green again |

### The criterion this host cannot evaluate, which was the other hole

`just test-determinism --compare-legs` compares digests published by several architectures. This
machine has one, so the ledger reports the criterion **NOT EVALUATED** rather than passed — and
`falsify.prove` refused to judge it for the same reason, correctly: a red produced by the laptop is a
verdict about the laptop. What that left was two of M11.a's seventy criteria that **nobody had shown
could fail**, and M11's repair gate was right to call that dispositive.

The missing piece was the **environment**, never the criterion. So a `where = "ci"` criterion may
declare the command that *constructs* what CI hands it:

```toml
[criterion.ci_proof]
provide = "python3 tools/ci/cross_leg_audit.py --claim lockstep --write-legs cross-leg-digests"
mutate  = "rename-token"
target  = "cross-leg-digests/cross-leg-digest-1/beta.digest"
token   = "44d0feadcb7cd95d"
```

`provide` is **executed**, not read, and the criterion's own body then runs verbatim three times: it
must pass against what `provide` produced, go red under the mutation *of that environment*, and come
back green once it is restored. A `provide` that supplies nothing leaves the criterion red at its
positive control and the verdict is `not provable here` — exactly the answer this shape replaced — so
there is nothing here that can be filled in falsely. The field is refused on any criterion that is
not `where = "ci"`, and refused outright beside `requires = "gpu"` or `requires = "display"`: no
shell command conjures a graphics device, and pretending otherwise would be this mechanism
committing the defect it exists to refuse.

**It claims nothing about the answer.** The criterion still reports NOT EVALUATED here and is still
answered only in CI. What it earns is the thing every other criterion on the ladder had and these two
did not: a demonstration that the check is capable of saying no.

**Why this is a ratchet and not a hole.** The defect runs in one direction: a criterion that is green
and that nothing can turn red. A criterion that is red is not that, and it is red in the open on
every run of its ledger. The day it goes green — which is what closing the rung means — the recorded
verdict stops matching the observed one, `reconcile` says so by name, and the criterion owes an
ordinary mutation proof before the ladder will take it again. **A rung cannot close by turning its
red criteria green quietly.**

**What keeps it honest is the tree control.** Red in the *sandbox* is not enough: the sandbox is a
copy of the tracked tree, so a criterion can be red in it for a reason that has nothing to do with
its subject — a generated header nobody commits, a `.git` that is not there. It has to be red in the
repository too. That control runs the criterion **unmutated** and nothing else; this tool never
mutates the working tree, which is the whole reason the sandbox exists.

**A criterion that needs a build** is `not provable here` unless a build is named, and then it is
*run* rather than argued about. `--build-dir`, or `CY_FALSIFY_BUILD_DIR` in the environment, which is
how `just roadmap-test` is told. A source-only run does not re-earn such a proof and does not destroy
it either: it reports that it could not judge it.

### The criterion that passes against a build, which was the hole

That used to be the edge of the tool, and the sentence it printed was *"turning it red needs its
source mutated and the tree rebuilt, which this prover does not do"*. **That sentence named sixteen
of M11.a's and M11.b's seventy criteria** — the GPU field sampler's consumers, the play-mode and
live-edit round trips, the gameplay and plugin suites, `lint`, `generated-code` — and it is a
description of the seven: green, with nothing in the tooling able to turn it red. A recorded reason
for not checking is still not checking.

So the rule that produced it — *never mutate the working tree* — is **narrowed rather than kept**.
`just roadmap-falsify prove --build-dir <dir> --mutate-the-tree` applies the mutation to the
repository itself, lets the criterion's own body rebuild over it (every one of the sixteen opens with
`just build-engine`), and requires three things in order:

1. **the positive control** — the criterion passes, unmutated, against that build;
2. **the mutation** — it goes **red**, with the rebuilt tree carrying the break;
3. **the restore** — the tree is put back and it comes back **green**. This is what separates *the
   mutation made it red* from *something about this run made it red*, and it is also how the restore
   is verified by something other than the restorer's own bookkeeping.

The ledger-blind control is not repeated here, and that is a rule rather than an omission: the shape
it exists to catch — a grep that matches the ledger declaring it — is refused *before any run*, by
the `self-match` rule in the table below.

**What makes it safe to point at the repository.** `--mutate-the-tree` is a flag on a command line,
never a default and never reached by `check`. It refuses to run inside another prover. Every byte it
overwrites is remembered first; the restore runs on the normal path, on an exception, on SIGINT and
SIGTERM, and at interpreter exit; and afterwards **`git status` must report exactly what it reported
before** — a byte-for-byte comparison against the index, made by a witness that took no part in the
bookkeeping. The baseline is *as this run found the tree*, not *as HEAD has it*, so a file that was
already modified is restored to the bytes it had and is never the target of the `git checkout`
fallback; neither is a file this run did not write to, because a prover takes minutes and an editor
does not stop for it.

**And one hazard a restore cannot undo, found on this tool's first run.** The working tree is put
back; a **commit taken while it was mutated** is not. An orchestrator snapshotting between phases
caught `src/rendering/sky/tests/test_cloud_shadows.cpp` with a renamed token in it and committed it —
after which the file on disk was right and HEAD was wrong, which is the one direction `git status`
reads as a stray modification to be tidied away. `HEAD` is therefore read before the mutation and
again after, a HEAD that moved **raises** rather than reports, and it is checked *before* the
`git checkout` fallback, which would otherwise faithfully put the mutation back.

**`plan-consistency` is the one criterion this prover cannot prove by running it**, because running
it runs the prover. `selftest.py` prints a line and skips its four sandbox-materialising cases when
`CY_FALSIFY` is set — which no pull request and no developer's `just roadmap-test` ever sets — so
what the criterion proves here is that `just roadmap-test` goes red when a ledger is broken, which
is the claim it makes.

### The mutation is derived, not described

The author writes nothing. A `path` criterion names the artefact, a `tiers` criterion names the
rows, and a text search names the token and the files it reads — so `falsify.py` derives the
mutation from the criterion's own text and applies it. A field an author fills in is a field an
author can fill in falsely, and prose is what the seven were written in.

Where a mutation cannot be derived — a loop over a shell variable, a path assembled by a command
substitution — the criterion declares one, and what it declares is a verb and a target the tooling
executes:

```toml
[criterion.falsifies]
mutate = "rename-token"   # delete-path | delete-lines | rename-token | truncate | lower-tiers
target = "src/editor/"
token  = "LiveEditPolicy"
```

A mutation that changes nothing is itself a finding: it names a file or a token that is not there.

**A declared gap is judged the other way round.** A criterion its ledger declares as an expected
failure (`known_gap`) is already red on the unmutated tree, in the open, on every run of that
ledger — "show that it can go red" asks for what the reader is looking at. What it has not shown is
that it is not *permanently* red, so its mutation is the gap's own closing act made small and must
take the criterion **green**. A mutation that leaves it red proves nothing and is reported as such.

### Which criterion evaluates which capability row

`delivery-roadmap` requires a recorded tier to be evaluated by a criterion, so a criterion says
which rows it evaluates in a field, not in prose:

```toml
evaluates = ["testing-and-quality"]
```

`criteria.evaluators(entries, row)` is the only way to ask, and `m11a:the-four-rows-are-evaluated` is
the guard that does. The field exists because the guard used to ask whether the row's name appeared
in a criterion's `source` — and `source` is a **citation**: eight criteria in M11.a's plan cite
`testing-and-quality` because that specification governs them. Deleting the criterion that actually
evaluated the row left the bystanders answering for it, and M11's gate found it: the guard "cannot
detect the deletion of two of the four evaluators".

### Shapes refused on sight

Some criteria cannot be judged by mutation because the sandbox has no build tree, and some cannot be
judged by anything because no state of the repository makes them fail. Those are read rather than
run, and each rule has a negative fixture in `selftest.py` spelled the way the ledgers spell it:

| Rule | Refuses |
|---|---|
| `searches-the-repository-root` | `grep -r <token> .` — the mere presence of the token **anywhere** satisfies it |
| `self-match` | a search that reaches `tools/roadmap/` without filtering it out: the ledger declaring the criterion is one of the files searched |
| `vacuous-suite` | `just test-render -R <suite>` and `ctest -R <suite>` — an empty selection exits zero unless the registration is asserted |
| `no-assertion` | a body whose every command is inert: it reports and returns |
| `swallowed-verdict` | `… || true` at the end: the exit code cannot reach the criterion |
| `absent-recipe` | the body runs a `just` recipe the justfile does not define: `just` aborts at argument parsing and nothing the criterion names is ever measured |

`absent-recipe` is the eighth defect of the seven, and the first one the prover **committed** rather
than caught. Three criteria ran `just quality-requirements <rows…>`; there was no such recipe, so
`just` stopped with *"Justfile does not contain recipes"*, ran nothing, and exited 1 — which is red
unmutated in the sandbox and in the repository, which is `red in the tree`, which is a **proof**. An
exit code cannot tell a subject failing from a name failing to resolve, so the shape is refused
before any run. The recipe those criteria were written for is `tools/roadmap/requirements.py`, and
its map is `tools/roadmap/requirements-coverage.toml`.

Two further shapes are reported apart, because they are a weaker complaint: `presence-only`, whose
whole verdict is that some text exists, and `artefact-presence`, a `path` criterion satisfied by a
file of the right name whatever is in it. Both *can* go red. Both are satisfied by typing the word.

### The list only shrinks

`falsifiability.toml` records every proof and every criterion not yet proven. `just roadmap-test`
re-runs the proofs rather than believing the file, and fails in four directions:

* a criterion nothing has judged — it is new, or edited since: it arrives with a proof or it does
  not arrive. **This is what stops the eighth.**
* an entry whose digest has moved. The digest is over what the criterion *checks*, so re-wording a
  description is free and changing a command costs a re-proof.
* an entry that has become provable — `DELETE THE ENTRY`, the same anti-rot direction as a declared
  gap that starts passing.
* a proof that has stopped proving.

The `roadmap-status` CI job checks out full Git history before this test. One recorded proof
references commit `ebdf8d5`; a shallow checkout cannot evaluate it and must fail the job's
historical-commit check instead of reporting an apparent criterion regression.

`--record` will always write a **proof**. It refuses to add a new *unproven* entry: otherwise it
would be the escape hatch that makes the whole mechanism advisory. The one exception is
`--baseline`, which wrote this ladder's existing debt once; it is a flag on a command line rather
than a field in a file, so using it shows up in a review.
When an existing `red in the tree` criterion changes, a declared gap that still fails in both the
sandbox and repository can refresh that proof's digest. This preserves only the observed claim that
the criterion goes red; it does not claim the missing work can already be completed.

**One check, one proof.** A proof `--record` writes for one ledger's declaration is written for
every other ledger's declaration of the same criterion id with the **same digest** — the same
command, artefact, expected tiers and declared mutation, byte for byte — and recorded `as
m1:four-profiles, the same check byte for byte`. `four-profiles` is declared by nineteen ledgers
with one body and evaluated once (`criteria.fingerprint`); the mutation that turned one copy red
turned the command every copy runs red, so the other eighteen are proven by it rather than sitting
on the unproven list as `not provable here`. It carries no further than that: edit one copy and its
digest moves, `just roadmap-test` flags it, and it owes a proof of its own.

## A gap a milestone ships knowingly

A criterion may carry two more fields, and they go together:

```toml
known_gap = "what is missing, and why the milestone shipped without it"
known_gap_closes = "m11"      # the rung that must close it
```

**Why this exists, and it was found rather than designed.** M8.c declared
`m8c:steam-audio-configures` *expecting it to fail* — deliberately, so the gap would keep saying so
on every run instead of quietly disappearing from the plan. That is the right instinct and it is
what `delivery-roadmap` asks for. But `just roadmap-milestone` returns non-zero for any failure,
`milestone-m8c` becomes a **permanent merge gate** the day it goes green, and `ci.yml` runs that
recipe — so flipping the gate would have shipped a continuous-integration job that can never pass.
That is the sibling of the forbidden pattern *"a milestone gate disabled rather than fixed or
explicitly superseded"*, and neither available answer was right: deleting the criterion hides the
gap, and an **override** is per-gate and would have hidden two hundred and fifty green checks to
excuse one.

So a declared gap is a third bucket, not a second kind of pass:

* it **runs** on every evaluation, like any other criterion;
* its failure is **printed**, under its own heading, with the rung that must close it;
* it does **not** set the exit code.

**And the direction that keeps it from rotting is the other one.** A declared gap that starts
PASSING fails the ledger, with `THE GAP IS CLOSED, DELETE THE DECLARATION`. A marker that outlived
its gap is exactly the thing nobody would notice, so the tooling notices. `just roadmap-test` covers
both directions, plus the three ways the declaration itself can be malformed: a gap with no rung, a
rung with no gap, and a rung that is not a milestone.

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

## Independent criteria run at the same time

`just roadmap-milestone <id>` evaluates its criteria **concurrently, up to a cap**, and M11.c's
ledger is 440 distinct checks of which 111 finish in under a second. Before this, every one of those
111 waited its turn behind a twenty-five-minute rebuild it has nothing to do with: `criteria.evaluate`
runs one criterion per `subprocess.run` and the caller was a plain `for` loop, on a machine with
twenty-four cores.

**The hard part is not running them at once; it is knowing which are independent.** Some criteria
configure and build a CMake tree, some run `ctest` in one, some drive Cargo, some want the one
graphics device or the one display, some bind a port, some write into the working tree. Two that
share any of those are not independent, and running them together produces a **flake** — which here
is strictly worse than being slow, because a verdict nobody can reproduce is a verdict nobody can
act on.

So **a criterion declares what it holds**, and `schedule.py` holds it exclusively for the duration:

```toml
[[criterion]]
id = "job-throughput"
run = 'd="${CY_BUILD_DIR:-build}/m1-bench"; CY_BUILD_DIR="$d" just test-bench-jobs --profile profile'
needs = ["build:m1-bench"]
```

A token is a resource class — `build`, `cargo`, `gpu`, `display`, `net`, `tree` — optionally with an
instance, so `build:m1-bench` and `build:off-ml` are two different trees and do not wait for each
other. `needs` is validated where `falsifies` and `evaluates` are, against a closed vocabulary and
for the same reason: **what is declared is acted on**, so it is a token rather than a sentence.

Almost no criterion declares one, because `schedule.derive` reads the answer off the criterion's own
body — but **only through facts it can see with certainty**:

- the `just` recipes it invokes, each classified in `RECIPE_NEEDS` (`build-engine` and every `test-*`
  recipe hold a build tree, because [every test recipe builds first](../../just/test.just);
  `quality-layers` holds nothing; `roadmap-falsify` can reach anything);
- the build-directory redirections it performs, each of which has to match a recognised idiom —
  `${CY_BUILD_DIR:+${CY_BUILD_DIR}/off-ml}` is one, and `CY_BUILD_DIR="$base/agree-$p"` inside a loop
  is deliberately not;
- its own `requires = "gpu"` or `requires = "display"`.

**Everything else is unknown, and an unknown criterion runs alone.** A shell function, a program not
in the table, a heredoc fed to an interpreter, a redirection into the working tree, a `rm`/`mkdir`/
`tee` whose target is an argument — each makes the body underivable, and an underivable criterion is
a barrier: nothing runs beside it, and it does not start until the pool has drained. Guessing is what
produces the flake, so nothing here guesses, and **declaring is how a criterion opts *into*
concurrency** rather than out of it. On M11.c's 440:

| | criteria |
|---|---|
| hold nothing — run with anything | 86 |
| share the ledger's own build tree | 223 |
| hold a build tree of their own (`off-ml`, `m1-bench`, `sanitize`, …) | 22 |
| hold Cargo, a port, the device or the display | 28 |
| **underivable — run alone** | **81** |

**The order of results does not change.** A reader compares one ledger run against the previous one
by eye, line by line, so the report is part of the contract: `schedule.run` returns one result per
entry in ledger order whatever order they finished in, and `roadmap._evaluate_plan` holds a finished
criterion's block back until every earlier one has been printed. Progress goes to stderr; the ledger
itself is on stdout and stays byte-comparable between a sequential run and a parallel one.

`--jobs 1` is the loop this replaced, kept so the two can be compared. `--jobs n` sets the cap;
without it, `CY_LEDGER_JOBS` or `min(8, cores)`.

### Proving it is safe, not merely faster

A speedup that changes one verdict is a regression, so the claim is checked in two places rather
than asserted.

`selftest.py` holds the scheduler's rules against fixtures, in milliseconds, on every pull request:
results come back in ledger order at every cap; nothing ever runs beside something it shares a
resource with; the cap is respected; a criterion that runs alone really does run alone and is a
barrier rather than starving; a declaration beats the derivation; every shape the derivation must
refuse is refused; every recipe `RECIPE_NEEDS` classifies is still a recipe the justfile declares;
and — the rule that would otherwise rot — **the derivation still reads most of the corpus**, because
a derivation that quietly stopped working would make every criterion exclusive, which is *correct*
and silently sequential.

`ledger_equivalence.py` runs the real thing:

```
python3 tools/roadmap/ledger_equivalence.py m11c --jobs 8
```

It runs the same ledger twice against the same tree, once with `--jobs 1` and once in parallel, and
compares the sequence of criterion labels, each criterion's verdict, and the whole report as text
with durations masked. Fixtures cannot show that the corpus's own bodies were classified correctly,
and the corpus cannot be run on every pull request; both are needed.

**Governed by**: `delivery-roadmap`, `developer-workflow-and-just`.

## Five criteria were half the ladder, and they were rebuilding each other's work

Five criteria configure and build the whole engine with a different option set each —
`m8c:feature-options-off`, `m8c:ml-option-off`, `m9:networking-option-off`,
`m9:networking-defaults-on` and `m9:multiplayer-profiles-agree`. They are the most expensive five
checks on the ladder by a wide margin — **98.6 minutes measured here**, against the 1.77 hours one
whole 435-criterion evaluation took at `2ca9e15` — and between them they performed **seven full
builds**.

The compiling was never the cost. These three were:

* **Seven builds for six configurations.** `networking-defaults-on` configures with no `-D` at all,
  and so does the Development half of `multiplayer-profiles-agree` — one configuration compiled
  twice, into two directories, because the two criteria live in two ledger files and neither could
  see the other's.
* **A different directory every run.** Every body spelled its tree
  `"${CY_BUILD_DIR:+${CY_BUILD_DIR}/off-ml}"`, hanging it under whatever the caller pointed
  `CY_BUILD_DIR` at — so a run under `build/m11c-ci` and the next under `build/m11d-crit` shared
  nothing and every evaluation was a cold build of the lot. **With `CY_BUILD_DIR` unset it was worse
  than cold**: the expression collapses to the empty string, `just build-engine` falls back to
  `build/dev`, and the option-off criteria reconfigured the developer's own tree with CY_VFX, then
  CY_ML, then CY_NETWORKING off — each forcing a full rebuild of the last one's work, and the last
  leaving `build/dev` in a state nobody asked for.
* **`rm -rf` on a tree that only needed a clean cache.** `networking-defaults-on` deleted its whole
  build tree every run. Its claim is about what a configure with no `-D` produces, and a remembered
  cache cannot answer that — but an object file remembers nothing about an option's default, so
  deleting them bought a full compile of the engine for information only the cache holds.

`matrix.py` is the answer to all three: **one stable tree per distinct configuration**, under
`build/ledger-matrix/`, named in one table that also says which criteria need each row.

```
$ python3 tools/roadmap/matrix.py list
dev-default      dev    no -D at all                 needed by m9:networking-defaults-on,
                                                               m9:multiplayer-profiles-agree,
                                                               m1:four-profiles …
debug-default    debug  no -D at all                 needed by m9:multiplayer-profiles-agree,
                                                               m1:four-profiles …
profile-default  profile no -D at all                needed by m1:four-profiles …
release-default  release no -D at all                needed by m1:four-profiles …
off-vfx          dev    -D CY_VFX=OFF                needed by m8c:feature-options-off
off-vulkan       dev    -D CY_RENDERER_VULKAN=OFF …  needed by m8c:feature-options-off
off-ml           dev    -D CY_ML=OFF                 needed by m8c:ml-option-off
off-networking   dev    -D CY_NETWORKING=OFF         needed by m9:networking-option-off
```

**Two more criteria joined it at M11.c's fifth close**, which took 7 h 28 m because every per-label
tree had been reaped and was built from empty: `m8b:feature-options-off` (rows `off-animation`,
`off-ai`, `off-ui`, `off-navigation`, built at once where they used to be built one after another)
and `m8c:steam-audio-configures` (row `on-steam-audio`). Each still checks what it checked: the same
`just build-engine` with the same options. Their run commands changed, so their proof digests moved.
Each declares its rows in `needs`, and `tools/ci/test_recipes.py` fails when a criterion builds a row
it does not declare or a row does not name a criterion that builds it.

**`m1:four-profiles` joined after M11.c's sixth close**, where it cost 1,187.9 s for one profile
and stopped there, because its four trees lived under `${CY_BUILD_DIR}/<profile>` and had been
reaped. Its dev and debug profiles ARE `dev-default` and `debug-default`; `profile-default` and
`release-default` are the two rows it added. The matrix builds the four at once, then the criterion
builds the editor and runs `just test-all` in each tree exactly as it did, stopping at the first
red. It is declared with a byte-identical body by nineteen ledgers, and the flattened ledger
evaluates it once only because the bodies are identical (`selftest.py`, "`four-profiles` is declared
by several ledgers and evaluated once"), so all nineteen were rewritten in one change; `selftest.py`
now also holds that the body names the four rows and no tree of its own, and `matrix.py` names every
ledger's copy in `needed_by` because `tools/ci/test_recipes.py` checks that ledger by ledger.

**The matrix is kept by `just build-reap`**, by name and by the `.cy-keep` marker `matrix.py` writes
at its root; `just roadmap-milestone` marks its own `CY_BUILD_DIR` the same way. It was a reap of
`build/m11c-final` between two closes that made the fifth one cold.

### What sharing may not mean here

Each of these criteria asserts **that the engine still configures and builds with an option off**.
That claim IS the build, so the matrix may not stand in for it. It does not cache an answer, does
not skip a configuration because a similar one passed, and never lets a criterion read a manifest
in place of a compiler: `ensure` runs `just build-engine` for every row it is asked for, **every
time it is asked**, and CMake and Ninja decide what is out of date — the rule `just/build.just`
already states for the recipe itself. What changed is *where* the build happens, not *whether*.

The one thing the matrix deletes is `dev-default`'s `CMakeCache.txt`, and only that row's, because
only that row carries a claim about what CMake computes when nothing is remembered. `CMakeFiles/` is
deliberately left alone: `cmake --fresh` would take it too, and under the Ninja generator that
directory holds the object files of every top-level target.

### Measured, on this workstation, both ledgers run in the same window

`criteria.evaluate` timed every row — the ledger's own clock, not a stopwatch around it. Both
columns were evaluated **at the same time on the same machine**, so they saw the same contention:
the criteria as they were, into `build/before-matrix`, and the criteria over the matrix, into a
matrix of their own. Every row PASSED in both.

**A matrix that does not exist yet** — which, before this change, is EVERY run, because
`${CY_BUILD_DIR}/off-ml` follows the caller's build directory and the caller's build directory is
new each time (`build/m11c-final`, `build/m11c-ci`, `build/m11d-crit`…):

| criterion | as it was | over the matrix |
|---|---|---|
| `m8c:feature-options-off` | 2055.9 s | **1585.0 s** |
| `m8c:ml-option-off` | 1064.6 s | **852.7 s** |
| `m9:networking-defaults-on` | 781.1 s | **780.7 s** |
| `m9:networking-option-off` | 863.6 s | **806.8 s** |
| `m9:multiplayer-profiles-agree` | 1151.5 s | **735.3 s** |
| **total** | **5916.7 s — 98.6 min** | **4760.6 s — 79.3 min** |

Seven builds became six, and the two `feature-options-off` needs are built side by side instead of
one after the other. `networking-defaults-on` is unchanged here and should be: on an empty tree
there is nothing for `rm -rf` to throw away.

**A matrix that already exists**, which is what the stable location buys and what every run after
the first one is:

| criterion | as it was | over the matrix |
|---|---|---|
| `m8c:feature-options-off` | 16.4 s | **7.2 s** |
| `m8c:ml-option-off` | 16.9 s | **16.7 s** |
| `m9:networking-defaults-on` | **516.8 s** | **30.8 s** |
| `m9:networking-option-off` | 16.4 s | **16.7 s** |
| `m9:multiplayer-profiles-agree` | 20.9 s | **27.5 s** |
| **total** | **587.4 s — 9.8 min** | **98.8 s — 1.6 min** |

Those two columns were taken in matrices of their own so that neither could warm the other. The
same five, run afterwards over the real `build/ledger-matrix` with nothing set in the environment at
all — which is what a ledger run finds — came to **104.2 s**, all green.

The 516.8 s is the `rm -rf`, and it is the whole of the difference: every other row was already
cheap once its tree existed, and no run before this change ever got to find that out.

So the five cost **98.6 minutes on a machine that has never evaluated this ledger, and 1.6 minutes
on one that has** — against 98.6 minutes every single time, which is what a per-run build directory
was buying.

### Each of the five was broken, and each went red

Not derived from the text: declared in the ledger as `[criterion.falsifies]`, applied to the working
tree by `falsify.py`, and rebuilt against a real matrix. Watched by hand first, at the same targets:

| criterion | what was broken | what the ledger said |
|---|---|---|
| `feature-options-off` | `CY_VFX` dropped from samples/08-vertical-slice's guard | red in 6.9 s — `Target "cy_sample_vertical-slice" links to cy::vfx but the target was not found`, the configure error naming the artefact that this criterion's own description forbids |
| `ml-option-off` | a `static_assert(false)` in `src/ecs/src/entity.cpp`, which the CY_ML=OFF tree compiles | red in 8.2 s |
| `networking-option-off` | `#error` unless `CY_NETWORKING` in `src/replay/src/readers.cpp` — the record put behind the transport's option | red in 8.0 s |
| `multiplayer-profiles-agree` | one line of output behind `#if defined(__OPTIMIZE__)` | red in 31.0 s — `0a1 > mutation.optimised = 1`, `Debug and Development disagree about the session`. BOTH trees built; what failed is the comparison, which is the half a shared matrix could have quietly collapsed |
| `networking-defaults-on` | `CY_NETWORKING|ON|` → `|OFF|` in cmake/features.cmake | red in 36.9 s — `a default configure left CY_NETWORKING off: CY_NETWORKING:BOOL=OFF`. That tree's cache said `ON` a minute earlier, which is the deletion of `CMakeCache.txt` doing exactly the work `rm -rf` used to |

Every mutation was restored and md5-verified against the bytes taken before it, and `git status` was
clean for the file after each one.

Then the same five were put through `falsify.py` itself — `prove --build-dir build/ledger-matrix
--mutate-the-tree --record`, which runs each criterion unmutated against a real matrix, applies the
declared mutation to the working tree, requires RED, restores, and requires GREEN again. All five
came back `proven against a built tree` and are recorded that way: **the unproven list shrank by
five**, which is the only direction it is allowed to move.

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

## An incremental close runs what a change can have moved, and never guesses

The full flattened ledger is the close: M11.c's took 9 824 s and M11.d's 6 744 s for 423 evaluated
criteria, most of which read nothing the change under review touched. `--incremental` evaluates
three sets instead and prints, for **every** criterion, whether it runs and why:

| set | selected when |
|---|---|
| **own** | always — the rung's own criteria are what it is closing |
| **smoke** | always — `m0:build`, `m0:format`, `m0:lint`, `m0:test`. A plan without all four is refused: a broken build or suite anywhere is not something an input set can rule out, and it is why a test criterion's inputs can leave out the rest of the build `_ctest` runs first |
| **new or edited** | its falsifiability digest (`falsify.digest`) differs from the one it had in the rung's plan at the base commit — the ledgers and `gates.toml` are read from that commit — or it was not in that plan at all, which is what a gate flipped green in between looks like |
| **inputs changed** | a path changed since the base (the working tree against it, both sides of a rename, and untracked files) is one of its inputs |
| **inputs unknown** | always. This is the rule the module is built around |

```
just roadmap-milestone m11d --incremental                          # against the last green full run
just roadmap-milestone m11d --incremental --changed-since main~3   # against a named commit
just roadmap-milestone m11d --incremental --list [--json]          # the selection alone, not run
```

**What "inputs" means, and where each answer comes from.** Nothing is curated by hand, because a
curated list goes stale in the unsafe direction:

- `tiers` — the status record and its parser. `path` — the criterion's glob, matched against a
  changed path or any directory above it.
- `recipe` and `command` — **only** a one-line body of `just <recipe>` invocations joined by `&&`,
  where each is a test recipe (`test-unit`, `test-integration`, `test-smoke`, `test-determinism`,
  `test-render`) given exactly `-R <regex>`, or `test-suites kind:regex…`, optionally behind
  `test-quiet-host --`. The tests that selects are read from `ctest --show-only=json-v1` in the build
  tree, and each one's inputs are: what its executable is built from (`ninja -t inputs`, then
  `ninja -t deps` for every header each object included); every repository path it names at run
  time — its objects' compile definitions, its command line, its environment, and its working
  directory when that is in the source tree — a build-tree path adding that output's own inputs; the
  CMake files of every directory above an input, `cmake/` and `CMakePresets.json`; and the recipe's
  own text — `justfile`, `just/*.just`, and the directory of every repository path the recipe's
  closure names in `just --show`.
- Every criterion also reads `tools/roadmap/criteria.py`, the evaluator.

**Unknown, and therefore selected**, with the reason printed: any other body (a multi-line script,
a pipe, a redirection, a variable); a recipe that is not a test recipe with `-R`; a test that runs
something the tree does not build (a Python script); a test that names the repository root or the
build tree's root; a regex that selects no test; an object with no recorded or a `STALE` header
record; and **a build tree that is not current** — `ninja -n` has work to do for the executable, so
its dependency log describes an older tree. Build it first (`just build-engine`, with the same
`CY_BUILD_DIR`) for a precise selection; `--build-dir` names another tree.

**One declared exception, pinned so it cannot outlive its review.** A compile definition naming
the repository ROOT means "this object can read any file", and `CY_DIAG_SOURCE_ROOT` is compiled into
the diagnostics library nearly every test links — so without an exception almost every test
criterion would be unknown. It is a prefix `sanitise_source_path()` strips with `memcmp`, never
opened; `incremental.toml` says so, names every file that uses the macro or calls `source_root()`,
and carries a digest of them. It **lapses** — the definition is a read again — when one of those
files changes or another file names a token, and the lapsed message prints the digest to write back
after re-reading them. `CY_SOURCE_DIR`, which `test_material_compile_service.cpp` really does
`fopen` under, has no entry and makes its tests unknown, as it should.

**Measured on a current `build/m11d-incremental-close`, against M11.d's 474-criterion ledger:**

| changed | selected | by inputs | skipped |
|---|---|---|---|
| `src/save/src/container.cpp` | 248 | 5 — the save criteria; the renderer's are skipped | 226 |
| `src/core/base/include/cy/core/base/types.h` | 417 | 174 | 57 |
| nothing | 243 | 0 | 231 |

The floor of about 240 is the rung's own 27, the smoke set, and **211 criteria whose inputs cannot be
read** — 141 multi-line shell bodies, and recipes such as `quality-requirements` or `run-sample` that
declare nothing. Every one of those is selected, every time; narrowing that floor is a matter of
criteria saying what they read, never of this tool guessing. The three selections above took 28.6 s
together, most of it reading the graph the first time.

**The trust boundary is Ninja's own.** A generator reading a file its build edge does not declare
would make Ninja skip a rebuild too; that is a build defect, and this mode inherits it rather than
second-guessing it. What it does not inherit is anything a test reads *at run time* by a path the
graph cannot see — which is why a test whose working directory is the repository root, or whose
definitions name it, is unknown rather than known.

**The baseline is only ever a green FULL run.** A full `roadmap-milestone <rung>` that exits 0, on a
tree that was clean and at the same HEAD from its first criterion to its last, records that HEAD in
`<git-common-dir>/cy-roadmap/last-green.json` — beside the object store, shared by every worktree,
never committed and never reaped. An incremental run never records one, so what it compares against
was always evaluated in full. With no record, `--changed-since` is required.

**The full ledger stays the default, runs nightly and at M11.e.** An incremental run is a fast
answer during a rung, not a close by itself, and its summary says so. The selection's four
properties — a change under `src/save` selects the save criteria and not the renderer's, a shared
header selects its dependents, an unknown-input criterion is always selected, and the rung's own
always are — are `test_incremental_selection` in `selftest.py`, each proven red against a mutation
of the code that provides it, and `m11d:incremental-close-selects-by-inputs` runs them and the real
mode over M11.d's own ledger.

**Governed by**: `delivery-roadmap`, `testing-and-quality` (Quality gates for merge).
