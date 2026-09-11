# Design: M9 — Integrity

## 1. The spike, and what it must answer before anything is written down

**M9's named risk is cross-platform floating-point determinism, and `docs/ROADMAP.md` already states
the shape of the answer: "It either holds under the declared profile or the profile's definition
changes."** The spike exists to make that a measurement rather than a preference, and it runs at the
head of the milestone, before `DeterminismProfile` has a definition anyone has coded against.

What it must answer, each as a number rather than a judgement:

| Question | Why it decides something |
|---|---|
| Do the engine's own math primitives agree bit-for-bit between clang 18 and GCC 13 at `-O0`, `-O1`, `-O2` and `-O3 -flto`? | If two compilers on ONE machine disagree, `CrossPlatform` is not about platforms and the profile's definition has to name a compiled subset |
| Does `<cmath>`'s transcendental set agree? | If it does not, the profile forbids them and the engine ships its own — a cost that has to be known before `simulation-and-determinism` claims Complete |
| Does the same question hold across x86-64 and ARM64? | **This machine cannot answer it.** It has one architecture, so the honest outcome is a profile verified where it was run and DECLARED UNVERIFIED elsewhere |

**The third row is why this section is written before the work rather than after it.** Every gate
this project has run found a claim exceeding what was checked, and "lockstep holds across two
platforms" is an exit criterion that cannot be evaluated on one machine. It is stated here so the
milestone plans for a criterion reported NOT EVALUATED — which the ledger already supports through
`requires` and `where = "ci"` — rather than discovering at its gate that it has to either weaken the
claim or fake it.

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
