# `src/ai/` — CyberAI

Agents as ECS entities, batched perception, the knowledge store, environment queries, smart objects,
AI level of detail and the deterministic think scheduler. `ai-system`, reaching **Working** at M8.b
(tasks 6.3 and 6.4).

## What is here, and the one thing that is not

`ai-system`: *"The AI system SHALL be engine code: the agent model, the graph compiler, perception
scheduling, the knowledge store, environment queries, smart objects, AI LOD, and the scheduler."*

All of it is here **except the graph compiler**, which is `cy::graph::behaviour` and belongs to
`visual-scripting` — the same line `src/animation/` draws between a runtime and a compiler. A
`BehaviourProgram` arrives here immutable and is shared by every agent running it; this module never
walks a graph.

*"WHEN the dependency manifest is audited THEN it SHALL contain no third-party game-AI framework."*
`cy::ai` links `cy::core`, `cy::ecs`, `cy::graph` and `cy::navigation` and nothing else.
`deps/manifest.toml` carries no AI library; the one dependency near this stack is Recast, which is a
navmesh **generator** and is declared as one.

`CY_AI` removes this directory and its suites. `-D CY_AI=OFF` still builds and tests navigation in
full, which is *"navigation SHALL remain fully functional for non-AI users"*.

## An agent is four components and a state block

| Component | What it carries |
|---|---|
| `AIAgent` | graph reference, importance, LOD tier, pin, gameplay-critical flag, seed |
| `AIState` | a one-based handle into the runtime's packed state table, plus the mirrored program counter, stack depth, status and last think tick |
| `Blackboard` | sixteen slots, resolved to indices at compile time |
| `PerceptionSensors` | vision, hearing, proximity, factions, acuity falloff |

What is deliberately **not** in a chunk is `cy::graph::behaviour::AgentState`, which owns a growable
blackboard array: one allocation per ECS row is the shape `ecs-core` exists to avoid. It lives in a
packed side table the runtime owns, and the blackboard rides in the chunk and is copied around a
think — sixty-four bytes each way, for an agent that actually thought.

**`kInvalidSlot` is zero, and that is load-bearing.** An ECS chunk is zero-initialised when an entity
is created, so a high sentinel would make a freshly-created agent look like the holder of slot zero
and silently execute another agent's program counter, stack and timers. Handles are therefore
one-based. `integration.ai_runtime`'s first case asserts it.

## There is no clock in this module

`ai-system` states the consequences of determinism rather than leaving them implied:

> An agent's LOD tier and think schedule SHALL be a function of **simulation state** — distance,
> importance, tick number, and a stable agent ordering — and SHALL NOT depend on measured frame time
> or thread timing.

So `AiRuntime::think()` takes a tick; the rotation is `tick % interval == index % interval`; the tier
policy reads distance and importance; the budget is a count of agents. Asynchronous knowledge arrives
on `post_tick + delay_ticks` in post order, and confidence decays by `(tick - last_tick) * rate` — so
ageing one tick at a time and ageing ten at once give the same answer, which `unit.ai` asserts.

**The two budget modes, plainly.** `BudgetMode::Deterministic` is the default: thresholds are fixed
configuration and an overrun is *reported*, never corrected by varying the schedule.
`BudgetMode::Adaptive` varies thresholds with measured load and **forfeits deterministic replay,
rollback and lockstep networking**. `AiRuntime::check_lockstep()` refuses the combination at
configuration time rather than at desync time.

`ThinkReport::state_hash` is the determinism test mode: a per-tick hash over each agent's entity,
program counter, world state and status, so a divergence names the tick and the agent.

## Perception is scheduled, batched, shared, budgeted and deferred

*"Agents declare sensors; they SHALL NOT issue their own per-agent queries during normal
operation."* `PerceptionScheduler::update()` is the pipeline the specification lists, in its order:
the sensors due on this tick, a broad phase by declared filter, the cheap rejections, **one** batched
call to `VisibilityHost`, then results into knowledge stores.

The host is an interface taking a **whole batch**, not one query — an interface that took one query
is an interface a caller will loop over, which is the shape this file exists to prevent. It is an
interface rather than a direct call into `cy::servers::physics` because a layer-4 module reaching
down to layer 2 would make every AI test need a physics world.

Sharing keys equivalent queries on (observer cell, target); the budget orders requests by importance
then by entity then by target; the tail past the budget is **deferred**, not dropped — the agent's
`last_sense_tick` is untouched so the rotation offers it again, and `PerceptionReport::deferred` says
how many. `sense_one()` is the direct path `ai-system` also requires, documented as expensive.

## Knowledge, not sensors

*"Behaviour graphs SHALL read knowledge, not sensors directly."* A `KnowledgeEntry` holds the last
known position and velocity, first and last perceived tick, a decaying confidence, a relevance and
the sensor that supplied it — which is what produces search behaviour instead of instant forgetting.
A `KnowledgeChannel` is a squad or a faction: posts are held for a configured delay and delivered
with a fidelity loss and a positional spread derived from the post itself, so a re-simulated tick
delivers the same approximate position.

## Suites

| Suite | Kind | What it holds |
|---|---|---|
| `unit.ai` | unit | the knowledge store and its decay, environment-query generators, tests, budget and ranking, smart objects and reservations |
| `integration.ai_runtime` | integration | the components, tiers and hysteresis, the rotation, the budget's deferral, promotion's reconstruction, the state hash, the decision history, and perception end to end |
| `integration.ai_scale` | integration | **eight thousand agents** thinking and sensing for a hundred and twenty ticks, and teardown under load |

`integration.ai_scale` reports the **median** tick and the worst beside it, never the best, and its
threshold is four times AI's declared share of a 60 Hz frame so that it detects a regression rather
than benchmarking the host. It also asserts that the work happened — the tier spread, the tasks run
and the traces issued — because a measurement of a loop that did nothing would also be fast.
