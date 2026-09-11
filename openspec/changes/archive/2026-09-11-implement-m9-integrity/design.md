# Design: M9 — Integrity

## 1. The spike, and what it answered

**M9's named risk is cross-platform floating-point determinism, and `docs/ROADMAP.md` already states
the shape of the answer: "It either holds under the declared profile or the profile's definition
changes."** The spike ran at the head of the milestone, before `DeterminismProfile` had a definition
anyone had coded against. It is `~/cyberdyne-spikes/m9-determinism-spike/` — outside the repository,
as M3's, M5.5's, M6's, M7's and M8.b's were, because a prototype under `docs/` fails
`just quality-layers`, correctly. `bash run.sh` reproduces every figure below.

**What it did**: built two probes in **twenty configurations each** — clang++ 18.1.3 and g++ 13.3.0,
at `-O0`, `-O1`, `-O2` and `-O3 -flto`, each with the engine's own `-ffp-contract=off` and again at
the compiler's default, plus `-march=native` at `-O2` in both — and bit-compared all forty runs.
Those four optimisation levels are **the four build profiles' own**: `cmake/profiles.cmake` compiles
Debug at `-O0`, Development and Profile at `-O2`, and Shipping at `-O3` with interprocedural
optimisation on, so the matrix answers the four-profile question directly rather than by analogy,
and adds `-O1` and explicit `-flto` beside them.
Probe one is sixteen workloads over the engine's own primitives **compiled from source** (vectors,
matrices, quaternions, transforms, projections, geometry, easing, curves, colour, the SIMD batch
paths against their scalar references, both random generators, `StateHashTree`, the spatial hash).
Probe two is thirty-eight `<cmath>` functions in `float` and `double`, read three ways.

![The M9 determinism spike's agreement matrix](../../../docs/design/images/m9-determinism-spike-matrix.png)

***`docs/design/images/m9-determinism-spike-matrix.png` — a DIAGRAM, not engine output***, and it
says so on its own face. Every cell is one workload's digest in one build, drawn from
`out/compare.txt` by the spike's `figure.py`; `run.sh` redraws it at the end of every run, so the
committed picture cannot describe a matrix that is no longer there. Same colour means bit-identical;
a numbered cell is a different result. The two columns that break the green are `-march=native` with
contraction left at the compiler's default — §1.2 — and the `cmath_*_folded` rows are §1.3.

### 1.1 The answers, as numbers

| Question | Answer |
|---|---|
| Do the engine's own math primitives agree bit-for-bit between clang 18 and GCC 13 at `-O0`, `-O1`, `-O2` and `-O3 -flto`? | **Yes — in 18 of 20 configurations, all 16 workloads, every value.** Both compilers, all four levels, contraction off *and* at each compiler's default, and `-march=native` *with* the flag |
| Then what are the other two? | **`-march=native` with contraction left at the compiler's default.** 13 of 16 workloads move; in 8 of those, clang and gcc disagree with *each other* as well as with the baseline. The deviation is a **median of one ulp** and it amplifies: up to 2 747 ulp in `vec`, 3 268 in `projection` and **12 681 163 in `curve`**, where a near-cancelling subtraction turns a last-bit difference into a different number. 26 780 of 72 704 values move in `matrix`, and 231 of 267 in `state_hash` — which is the hash a lockstep session compares |
| Does `<cmath>`'s transcendental set agree? | **At runtime yes, when folded no.** All 20 builds agree on every runtime call. Thirteen functions change value when the compiler evaluates them itself: `acos acosh asin asinh atan2 atanh cbrt cosh expm1 log10 log1p sinh tgamma` |
| Does the same question hold across x86-64 and ARM64? | **NOT EVALUATED — see §1.4.** This machine has one architecture and one operating system |

### 1.2 The finding that changes something: contraction, not the compiler

`src/core/math/CMakeLists.txt` passes `-ffp-contract=off` to `cy_core_math`, `PRIVATE`, with a
comment calling it load-bearing for the SIMD-versus-reference bit-identity test. **The spike turns
that comment into a number, and then finds the hole beside it.**

The eighteen agreeing configurations include the ones *without* the flag — but only because the
engine targets baseline x86-64, which has no FMA, so there is nothing for the default setting to
fuse. Give both compilers a target that does have FMA and the flag is the only thing holding the
result still. **That is not a hypothetical target**: `src/core/math/tests/CMakeLists.txt` already
anticipates `-march=x86-64-v3` in as many words.

And the flag is set on **one** of the engine's 81 modules, and `PRIVATE`. Physics, animation,
navigation, gameplay and the ECS all do floating-point arithmetic in their own translation units
with contraction at the compiler's default. On today's baseline that costs nothing; on the day the
baseline moves it costs the milestone's headline claim, silently, in a build that still passes every
test the tree has.

**So the profile is a build-configuration contract, not only a source contract.** `DeterminismProfile`
cannot be a property of code alone: two builds of identical source, differing only in `-march` and a
contraction flag, produce different state hashes. Section 3's configuration-time refusal has to see
the compile options, and the determinism lint (task 2.6) has a second job beside reading source —
asserting that every module a profile covers was compiled with contraction off.

### 1.3 `<cmath>`: forbid the fold, not the function

The runtime half agrees across all twenty builds, which is expected on one machine — both compilers
link the same glibc 2.39 — and is therefore *weak evidence*, worth stating only because it rules out
the compilers substituting different inline code.

The folded half does not agree, and that half ships. A game's tuning constants are compile-time
constants; `std::cos(kQuarterPi)` in a header is a folded call. GCC folds through MPFR from `-O1`,
Clang mostly declines to fold in `double` and does fold in `float` from `-O1`, and glibc computes at
runtime — three evaluators for one expression, and at `-O0` neither compiler folds, so **`-O0` and
`-O2` of the same source disagree**. Thirteen functions move by one to three ulp.

**And the two halves corroborate each other exactly.** The probe separately compares each function
against the same computation carried out in `long double` and rounded back. The functions whose
folded value moves are, point for point and count for count, the functions this libm does not round
correctly — `cbrt` 12 of 24 sample points in both tables, `acosh` 7, `tgamma` 5, `sinh` 4, `asinh` 3,
`cosh` 2, `atanh` 2, `expm1`/`log10`/`log1p` 1 each; and the same ten agree count-for-count in the
`float` column too. Two functions are in one table and not the other, and neither weakens the
match: `atan2` folds but takes two arguments, and the wider-precision column covers the unary set
plus `pow`; `lgamma` is flagged by that column and neither compiler folds it. A function that is not
correctly rounded has no unique right answer, so its value is a property of *this* libm build rather
than of IEEE-754 — which is exactly the value a different distribution, a different glibc or a
different architecture is free to change.

**The consequence for the profile, stated so tasks 2.1–2.3 do not re-derive it:**

- `sqrt`, `fabs`, `floor`, `ceil`, `trunc`, `round`, `nearbyint`, `fma`, `fmod`, `remainder`,
  `copysign` are **exact by IEEE-754** and measured exact here. They stay.
- `exp`, `exp2`, `log`, `log2`, `sin`, `cos`, `tan`, `atan`, `tanh`, `erf`, `pow` scored **zero**
  against the wider reference on every sample point. They are correctly rounded in this libm — which
  is a statement about glibc 2.39, not about the standard, so a `CrossPlatform` profile may not rest
  on it.
- The thirteen above are **forbidden to an authoritative path under any profile**, and the engine
  ships its own where the simulation needs them. That is §1.5's cost, and it is known now rather
  than at the gate.

### 1.4 What this machine cannot answer, and how the ledger must say so

**One architecture, one operating system**: x86-64, Linux 6.8.0, glibc 2.39, on a single i9-12900K.
Everything above is a statement about *two compilers on one machine*. `docs/ROADMAP.md`'s exit
criterion "**lockstep holds across two platforms for the `CrossPlatform` profile**" cannot be
evaluated here and **must never be reported as a pass**.

The ledger already has the mechanism and M3 has three criteria using it correctly — `conventions`,
`golden` and `vulkan-frame` carry `requires = "gpu"` with a `reason`, and `three-platforms` carries
`where = "ci"` with a `reason`. `tools/roadmap/criteria.py` rejects either without a `reason`, and
`unmet_requirement()` then reports the criterion NOT EVALUATED rather than passing it.

So M9's lockstep criterion is declared `where = "ci"` with a reason naming this limit, in the shape
of m3.toml's `three-platforms`:

```toml
[[criterion]]
id = "lockstep-cross-platform"
describe = "a lockstep session under the CrossPlatform profile reaches the same state hash on two architectures"
source = "ROADMAP M9, tasks 3.3, 0.3"
kind = "command"
run = "..."
where = "ci"
reason = "this host has one architecture and one operating system; the second platform exists only in the CI matrix"
ci_job = "..."
```

`REQUIREMENTS` in `tools/roadmap/criteria.py` is `("display", "gpu")` — there is no `"arch"`, and
adding one is not this milestone's business. `where = "ci"` is the mechanism that already fits.

**And what CI can settle, checked rather than assumed.** `.github/workflows/ci.yml`'s `build` and
`test` jobs are a **six-leg matrix that already includes ARM64** — `linux-arm64` on
`ubuntu-24.04-arm`, `macos-arm64` on `macos-14`, `windows-arm64` on `windows-11-arm`. So the
cross-architecture question is answerable *there* and only there, which makes `where = "ci"` the
right mechanism rather than a polite way of never checking: the criterion runs where a second
architecture exists and reports NOT EVALUATED on this host, which is exactly what it is.

Two conditions the implementing agent must confirm rather than inherit. First, a criterion that
compares a state hash **between two legs** needs the legs to exchange an artefact — the matrix runs
them independently — so the shape is "leg uploads its hash, a following job compares", not a single
`run` line. Second, if that turns out not to be buildable inside M9, the honest record is a
**`known_gap` with a rung that must close it**, never a criterion quietly reported green. Either
way, `docs/roadmap/status.yaml` must not record `simulation-and-determinism` as claiming
cross-architecture determinism until a run has compared two architectures, because nothing in this
tree has done so yet.

### 1.5 What the spike cost the plan

Nothing in §2–§5 changes. The milestone's scope survives its own spike, because the measurement came
out the way the profile wanted on the configuration the engine actually builds. What it adds is
three obligations that would otherwise have been discovered late:

1. **The profile covers build configuration, not only source** (§1.2). Task 2.2's refusal and task
   2.6's lint both gain a compile-options input.
2. **Thirteen named `<cmath>` functions are forbidden to authoritative paths** and the engine ships
   its own (§1.3). That is real work inside task 2.3, and the thirteen are now named rather than
   guessed at.
3. **One exit criterion is reported NOT EVALUATED** through `where = "ci"` (§1.4), planned at
   proposal time rather than argued at the gate.

### 1.6 The instrument was wrong first

Recorded because M8.b's spike is the model and because this project's last thirteen gates found
eighteen of twenty-two defects in the checking rather than in the engine.

**The spike's first full run reported seven of fifteen workloads diverging between clang and gcc** —
at index 0, by whole numbers rather than by ulps. It was not a finding about the engine. Two draws
inside one function-call argument list are **unsequenced** in C++: clang evaluates them right to
left and gcc left to right, so the two compilers were fed *different inputs*. Every diverging
workload was one with such a pair, and every agreeing workload had none.

The first repair was a digest over the input stream, and **it did not work** — the stream delivers
the same values in the same order whoever consumes them, and only the pairing of consumer to value
changes, which a generator cannot see. The check that does work is static: `lint_draws.py` fails if
any full-expression draws twice, and `run.sh` refuses to build anything until it passes. Restoring
the single offending line makes the lint name it — `probe_math.cpp:331: 2 draws in one
full-expression` — and makes `run.sh` print *"THE INSTRUMENT IS UNSOUND — no measurement taken"* and
exit 2. **That mutation is the proof it is a check rather than a decoration**, and it is the rule
this milestone's own criteria are held to.

## 2. One log, and the four readers that are not allowed to fork it

The milestone's subtitle is "one command log, read five ways", and the failure it is guarding
against is the obvious one: replay grows a log, replication grows a second, the crash buffer grows a
third, and the three drift until a replay of a networked session is a different program from the
session.

So the log is written once, by `cy::gameplay::CommandStream`, and the five readers are:

1. **Replay** — playback and seeking against checkpoints.
2. **Rollback** — re-simulation from a checkpoint, with the **side-effect ledger** deciding what must
   not run twice.
3. **Replication** — the authority model's inputs, in the modes that send inputs.
4. **The crash replay buffer** — a bounded ring of the same records, flushed into the crash artefact.
5. **The validator** — the divergence localiser, which reads the log beside two state hashes.

**What this milestone must not do is give any of the five its own record type.** M8.b's spike found
that one IR cannot serve seven consumers and M8.c honoured that by giving each consumer its own
lowering; the opposite lesson applies here and it is not a contradiction. A graph IR is an
*execution form* and five consumers execute differently. A command log is a *record of what
happened*, and five records of what happened that disagree is the bug.

## 3. The determinism profile is a configuration-time contract

`simulation-and-determinism`'s exit criterion is the strong one: "A session declaring a determinism
profile a subsystem cannot meet is **rejected at configuration**, not discovered later." That is the
same shape as M8.c's cook-time firewall and it is picked for the same reason — a diagnostic that
fires at runtime has already shipped the defect.

Concretely, each subsystem that participates declares what it can meet, the session declares what it
requires, and the mismatch is a configure-time or session-creation-time refusal naming the subsystem
and the guarantee. The validator and the lint are the two halves that keep that declaration honest
afterwards: the lint is static and finds the code that could not meet it, and the validator is
dynamic and finds the field that did not.

## 4. Which of the six Complete rows are actually small, stated rather than assumed

Six Complete rows is the largest load any milestone in this plan carries, and three of the six
capability-matrix gates before this one demoted a Complete cell rather than accept it
(`text-and-fonts` at M8.b, `audio` at M8.c). **The honest position at proposal time is that three of
these six are genuine work and three are contingent on it:**

- `simulation-and-determinism`, `replay-and-rollback` and `networking-and-replication` are the
  milestone. Everything else here is downstream of them.
- `gameplay-framework` reaches Complete *because* replication and replay exist — its remaining
  requirements are "network integration, save and replay contracts, headless operation". If the two
  systems above land, this row is integration; if they do not, this row does not move and saying so
  early is cheaper than saying it at the gate.
- `save-and-persistence` needs integrity and confidentiality over a container that already exists,
  and checkpoints that the replay work produces anyway.
- `diagnostics-profiling-and-crash` needs the crash artefact to carry the replay buffer, which is
  one of the five readers above.

**The demotion this design predicts, if one is needed, is `networking-and-replication`.** It is the
only row here that names a transport, a dedicated server and lag compensation in one breath, and it
is the row a reduced milestone should split rather than the row that should be claimed thin.

## 5. What this milestone deliberately does not do

- **No new graph IR and no new authoring layer.** CyberGraph and the per-consumer lowerings are
  M8.b's and M8.c's settled answer.
- **No relitigating the firewall's enforcement point.** M8.c decided it at the ECS write path and
  proved it by four source mutations. Replication and rollback read provenance; neither may make a
  command's provenance affect its validation, ordering or execution, which `gameplay-framework`
  forbids in as many words.
- **No second determinism mechanism beside `Classified<>`.** It is the compile-time half and the
  firewall is the runtime half; a third would be a third thing to keep in step.
