# Design: M10 — Worlds

## 1. The spike, and what it must answer before section 4 is written

**M10's named risk is region invalidation in PCG**, and `docs/ROADMAP.md` states the consequence:
getting dependency-driven partial regeneration wrong means either stale content or full-world
regeneration, and both are project-defining. The spike runs at the head of the milestone, before
`procedural-content-generation`'s graph has a dependency model anyone has coded against, and it lives
outside the repository the way M3's, M5.5's, M6's, M7's, M8.b's and M9's did — a prototype under
`docs/` fails `just quality-layers`, correctly.

**What it must answer, as numbers rather than as a position:**

| Question | Why it decides something |
|---|---|
| Does a dependency-driven partial regeneration of a region reproduce the full regeneration of the same seed, bit for bit, including generated identity? | If not, caching is unsound and the graph's edges are wrong; everything in section 4 rests on this |
| What is the blast radius of one edit, measured in regions, for a graph with realistic edges? | A partial regeneration that touches the world is a full regeneration with extra bookkeeping |
| Does a hand-placed override survive the regeneration of its region? | It is an exit criterion, and an override model chosen after the cache is chosen is an override model that loses |

**And what it cannot answer here** is reported through the ledger's `requires`/`where` mechanism
rather than asserted: this host has one architecture and one GPU vendor, so any claim about a second
is NOT EVALUATED, never a pass. M9's `lockstep-cross-platform` is the shape.

## 2. One substrate, and the producers that are not allowed to fork it

M9's subtitle was "one command log, read five ways" and its design named the failure it guarded
against: five readers growing five records of what happened. **M10's version of that failure is
seven producers growing seven fields that mean the same thing** — terrain writing a wetness it
computed, weather writing another, foliage sampling a third.

So the substrate is declared once, and **one producer per field** is enforced at registration with a
refusal that names both producers. That is the same shape as M9's configuration-time determinism
refusal and M8.c's cook-time refusal, and it is picked for the same reason: a diagnostic that fires
at runtime has already shipped the defect.

The consumers — gameplay, rendering, audio, navigation — read fields. A consumer that writes one is
the bug this milestone is shaped to make impossible rather than to detect.

## 3. Determinism is inherited, not re-invented

`simulation-and-determinism` reached Working at M9 with a classification model, a firewall at the ECS
write path and a lint. **M10 adds no second mechanism.** A gameplay-visible field is authoritative
state and is classified as such; a presentation-only field is firewalled by the machinery that
already exists; `procedural-content-generation`'s "deterministic derivation" is `random.h`'s
counter-based streams and nothing else.

The one thing M10 owes determinism is **`tests/determinism/`**, which M9's own gate found empty:
`tests/CMakeLists.txt` promises the kind joins the taxonomy at M9 and `testing-and-quality` gives it
a location and a 10 s budget. Golden replays, replay and save fuzzing and the transactional save
tests live there, and a procedurally generated world is the first workload large enough for a golden
replay to be worth committing.

## 4. Which of these rows are actually contingent, stated rather than assumed

Eight rows to Working and three to Complete is a large load, and the honest position at proposal
time is that they are not equally risky:

- `environment-fields` is the milestone. Every other world row writes into it, and a substrate
  designed after its first two producers exist is a substrate with two special cases in it.
- `terrain`, `water` and `atmosphere-sky-and-clouds` are large but well-understood: each has a
  specification with a settled shape and no dependency on the spike's answer.
- `procedural-content-generation` is the row that depends on section 1. **If the spike says partial
  regeneration cannot be made to reproduce the full one, this row's scope changes** — caching and
  spatial invalidation come out, and the row reaches Working with regeneration that is correct and
  slow rather than fast and unsound.
- `foliage` and `weather-and-wind` are downstream of the substrate and of PCG's placement.
- **The three Complete rows are the ones M9 demoted, and each is blocked on a named, running
  criterion rather than on a design question.** If those criteria do not go green, the rows do not
  move, and a second demotion of the same row means the row is mis-scoped rather than late — that is
  a finding about the plan, and it belongs in the next milestone's proposal rather than in its gate.

**The demotion this design predicts, if one is needed, is `procedural-content-generation`** — it is
the only row here whose scope is decided by a spike that has not run yet.

## 5. What this milestone deliberately does not do

- **No new renderer architecture.** The render graph, the material compiler and the shader system are
  M3's, M7's and M8.b's settled answers; volumetric clouds and water shading are passes in the graph
  that exists.
- **No second determinism mechanism**, and no relitigating the firewall's enforcement point. M8.c
  decided it at the ECS write path and proved it by mutation; M9 armed it.
- **No cross-platform claim.** `simulation-and-determinism`'s Complete cell sits at M11 with the
  deterministic math module, because the guarantee it turns on cannot be measured on one
  architecture, and M9's gate refused to claim it on a single host.
