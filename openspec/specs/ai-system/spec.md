# ai-system Specification

## Purpose

Defines **CyberAI**: agent decision-making, perception, knowledge, and world interaction, designed
for tens of thousands of thinking agents.

Agents are ECS entities processed in bulk — the opposite of the UI case, because this workload is
exactly what archetype storage and declared-access parallelism are good at. A single **AI graph**
composes hierarchical state trees, behaviour trees, utility scoring, and GOAP planning in one
asset with one editor and one debugger, **compiled** to a compact program shared by every agent
using it. Perception is **scheduled globally and batched**, rather than every agent issuing its own
raycasts. Agents hold **knowledge with decaying confidence** rather than answering `canSeeEnemy`,
which is what produces search behaviour instead of instant forgetting.

The defining constraint is stated as a requirement rather than left implicit: **AI is
deterministic**, because it drives gameplay that must survive network reconciliation and replay.
That directly constrains frame-budget adaptation — the schedule is a function of simulation state,
never of measured frame time — and the two available budget modes make the trade-off an explicit
project decision rather than a discovery during multiplayer testing.

Scale comes from **AI LOD**: tiers reduce not only think frequency but reasoning fidelity, down to
a statistical population model for distant crowds.

## Requirements

### Requirement: Engine-owned AI architecture
The AI system SHALL be engine code: the agent model, the graph compiler, perception scheduling,
the knowledge store, environment queries, smart objects, AI LOD, and the scheduler.

Proven algorithms MAY be integrated where they are not differentiating — navmesh generation and
pathfinding continue to use the navigation stack's dependencies. The scheduler, LOD policy, graph
compiler, perception batching, knowledge model, and smart objects SHALL be engine-owned.

The system SHALL be removable at build time via `CY_AI`.

#### Scenario: No AI framework dependency
- **WHEN** the dependency manifest is audited
- **THEN** it SHALL contain no third-party game-AI framework

#### Scenario: AI disabled
- **WHEN** `CY_AI` is disabled
- **THEN** the AI runtime SHALL be excluded, and navigation SHALL remain fully functional for
  non-AI users

### Requirement: Agents are ECS entities
An AI agent SHALL be an ECS entity with components — at minimum `AIAgent` (graph reference,
importance, LOD tier), `AIState` (program counter, stack, timers), `Blackboard`, and optionally
`PerceptionSensors`, `Knowledge`, and `NavAgent`.

Agent behaviour SHALL execute as scheduled systems over queries, in bulk. The engine SHALL NOT
provide a per-agent virtual update object.

Agents sharing a graph SHALL share one compiled program and differ only in per-entity data.

#### Scenario: Bulk execution
- **WHEN** 10,000 agents run the same graph
- **THEN** they SHALL be processed by systems iterating packed component arrays, sharing one
  compiled program

#### Scenario: Agent composition
- **WHEN** a designer creates an agent type
- **THEN** it SHALL be an archetype template plus a graph asset, not a class in a hierarchy

#### Scenario: Agents participate in scheduling
- **WHEN** AI systems declare their component access
- **THEN** the ECS scheduler SHALL parallelise them against other systems by the same rules as any
  other system

### Requirement: Unified AI graph
A single **AI graph** asset SHALL be able to compose four reasoning models, nested arbitrarily:

| Model | Expresses |
|---|---|
| **Hierarchical state tree** | Mode and transitions, with nested states and enter/exit actions |
| **Behaviour tree** | Ordered selection and sequencing with decorators and services |
| **Utility scoring** | Competing continuous needs, scored and selected |
| **GOAP planning** | Goals with preconditions and effects, planned into action sequences |

A state MAY contain a behaviour tree; a behaviour tree node MAY be a utility selector; a utility
action MAY be a GOAP goal; and each MAY contain the others.

The graph SHALL have one editor, one debugger, and one asset type.

#### Scenario: Composed reasoning
- **WHEN** a `Working` state contains a utility selector whose winning action is a GOAP goal
- **THEN** the graph SHALL execute state, scoring, and planning in one coherent program

#### Scenario: The right model for the problem
- **WHEN** an agent has several competing continuous needs
- **THEN** the author SHALL be able to express it as utility scoring rather than as a large
  behaviour tree encoding the same comparison

#### Scenario: One debugger
- **WHEN** an agent misbehaves
- **THEN** the debugger SHALL show its active state, behaviour tree node, utility scores, and
  current plan together, not in separate tools

### Requirement: Behaviour tree semantics
Behaviour tree nodes SHALL include: `Selector`, `Sequence`, `Parallel`, decorators (inverter,
succeeder, repeater, cooldown, condition guard), `Condition`, `Task`, `Service` (periodic
background work while a subtree is active), and `Subtree` reference.

Node status SHALL be `Running`, `Success`, or `Failure`, with execution resuming at the running
node rather than re-descending from the root each tick, unless a guard invalidates it.

#### Scenario: Execution resumes
- **WHEN** a task returns `Running`
- **THEN** the next think SHALL resume at that task without re-evaluating the whole tree, unless a
  condition guard above it has been invalidated

#### Scenario: Guard aborts a subtree
- **WHEN** a condition guard becomes false while its subtree is running
- **THEN** the subtree SHALL be aborted with the configured abort semantics, and the running task
  SHALL be notified so it can clean up

### Requirement: Utility scoring
Utility actions SHALL declare a scoring function over agent and world context, producing a score;
the highest-scoring eligible action SHALL be selected.

The system SHALL support: per-action weight and cooldown, response curves (linear, quadratic,
logistic, inverse) mapping inputs to scores, multiplicative and additive combination of
considerations, a momentum or hysteresis factor preventing oscillation between near-equal
actions, and score inspection for debugging.

#### Scenario: Competing needs
- **WHEN** an agent has low energy, full cargo, and a nearby enemy
- **THEN** each candidate action SHALL be scored and the highest selected, with the scores
  inspectable

#### Scenario: No oscillation
- **WHEN** two actions score nearly equally across successive thinks
- **THEN** hysteresis SHALL prevent the agent from switching between them every think

### Requirement: GOAP planning
The system SHALL provide goal-oriented action planning: goals with a desired world state, actions
with preconditions, effects, and costs, and a planner producing a valid action sequence.

Planning SHALL support: a maximum search budget, incremental planning across ticks for large
plans, plan invalidation when preconditions become false, and replanning.

Plan search SHALL be deterministic: the same world state and action set SHALL produce the same
plan.

#### Scenario: Plan is produced
- **WHEN** an agent adopts a goal with a valid action sequence available
- **THEN** the planner SHALL produce that sequence within its search budget

#### Scenario: Replanning
- **WHEN** a precondition of a step in the current plan becomes false
- **THEN** the plan SHALL be invalidated and replanning triggered, rather than executing an
  invalid step

#### Scenario: Budget exceeded
- **WHEN** planning exceeds its search budget
- **THEN** it SHALL report failure deterministically and the graph SHALL take its configured
  fallback, rather than stalling

### Requirement: Compiled behaviour programs
AI graphs SHALL be **compiled**, not interpreted node-by-node at runtime.

Compilation SHALL produce a compact program — a flat instruction stream plus a parameter table —
shared by all agents using that graph. Per-agent runtime state SHALL be a small block: program
counter, execution stack, timers, and blackboard.

Compilation SHALL occur at cook time, with the optimiser performing at minimum: constant folding
of authored parameters, dead-branch elimination, and condition hoisting where a condition is
invariant within a subtree.

#### Scenario: Program is shared
- **WHEN** 10,000 agents use one graph
- **THEN** one compiled program SHALL exist, and per-agent memory SHALL be the state block only

#### Scenario: Compiled at cook time
- **WHEN** a game ships
- **THEN** it SHALL contain compiled programs and no graph compiler

#### Scenario: Compile error is actionable
- **WHEN** a graph fails to compile
- **THEN** the error SHALL identify the node and pin, not only a program offset

### Requirement: Blackboard and context
Each agent SHALL have a **blackboard**: typed named values readable and writable by graph nodes,
with declared keys, types, and default values.

Blackboard keys SHALL be resolved to indices at compile time; runtime access SHALL NOT be a string
lookup.

The graph SHALL additionally have read access to a **context**: the agent's own components, its
knowledge store, and declared world state, without copying them into the blackboard.

#### Scenario: Compile-time key resolution
- **WHEN** a node reads a blackboard key
- **THEN** it SHALL access a resolved index, with no string hashing at runtime

#### Scenario: Undeclared key
- **WHEN** a graph references a key not declared on its blackboard
- **THEN** compilation SHALL fail naming the key and node

### Requirement: Batched perception
Perception SHALL be scheduled globally. Agents declare sensors; they SHALL NOT issue their own
per-agent queries during normal operation.

Each tick the perception scheduler SHALL: gather sensors due for update, perform broad-phase
candidate filtering using spatial partitioning and declared filters (faction, range, layer),
apply cheap rejections (distance squared, view angle, cached occlusion), batch the surviving
visibility queries against the physics server, and write results into knowledge stores.

Query results SHALL be shareable between agents where the query is equivalent.

The scheduler SHALL enforce a per-tick query budget, prioritising by agent importance.

Sensor kinds SHALL include at least: vision (range, field of view, and acuity falloff), hearing
(range and loudness attenuation), damage, touch, proximity, and a registration point for custom
sensors.

A direct, unbatched query path SHALL remain available for cases needing exact instantaneous
results, documented as expensive.

#### Scenario: Batching replaces per-agent raycasts
- **WHEN** 10,000 agents have vision sensors
- **THEN** the scheduler SHALL issue a bounded, batched set of queries rather than one or more
  raycasts per agent per tick

#### Scenario: Shared query result
- **WHEN** several nearby agents would test visibility to the same target
- **THEN** the result MAY be computed once and shared, subject to the sensors' parameters being
  compatible

#### Scenario: Query budget
- **WHEN** perception demand exceeds the tick's query budget
- **THEN** queries SHALL be prioritised by agent importance and the remainder deferred to
  subsequent ticks deterministically, with the deferral reported

#### Scenario: Results are tick-quantised
- **WHEN** a target becomes visible between perception updates
- **THEN** the agent SHALL perceive it at the next scheduled update, and this latency SHALL be
  documented rather than treated as a defect

### Requirement: Knowledge and memory
Each perceiving agent SHALL maintain a **knowledge store** of perceived entities, holding at
minimum: entity reference, last known position and velocity, first and last perceived time,
a **confidence** value, a threat or relevance assessment, and the sensor that supplied it.

Confidence SHALL **decay** over time when the entity is not re-perceived, at a configurable rate,
and entries SHALL be forgotten below a threshold or evicted when the store is full, by lowest
relevance.

Behaviour graphs SHALL read knowledge, not sensors directly.

Knowledge SHALL be shareable between agents through a declared channel (a squad, a faction), so
one agent's perception can inform others, with configurable delay and fidelity loss.

#### Scenario: Target breaks line of sight
- **WHEN** a perceived enemy moves out of view
- **THEN** the agent SHALL retain its last known position with decaying confidence, enabling
  search behaviour rather than instant forgetting

#### Scenario: Confidence drives behaviour
- **WHEN** confidence in a target's position falls below a threshold
- **THEN** the graph SHALL be able to branch on that, for example from pursuing to searching

#### Scenario: Squad awareness
- **WHEN** one squad member perceives an enemy and knowledge sharing is enabled
- **THEN** other members SHALL receive it with the configured delay, rather than instantly and
  perfectly

### Requirement: Environment queries
The system SHALL provide **environment queries**: spatial reasoning queries that generate candidate
points or actors, score them by weighted tests, and return the best or a ranked set.

Generators SHALL include at least: points on a grid, points on a circle or donut, points on the
navigation mesh, points around an actor, and actors of a type in range.

Tests SHALL include at least: distance, dot product and angle, line of sight, navigation
reachability and path cost, height difference, overlap, and a registration point for custom tests.

Each test SHALL support scoring (with a response curve) and filtering, and queries SHALL declare a
budget so cost is bounded.

Queries SHALL be issuable asynchronously with deterministic completion.

#### Scenario: Finding cover
- **WHEN** an agent queries for cover
- **THEN** the query SHALL generate candidate points, score them by distance, line of sight to the
  threat, and reachability, and return the best

#### Scenario: Query budget
- **WHEN** a query's candidate count exceeds its budget
- **THEN** candidates SHALL be reduced deterministically before scoring, and the reduction reported

### Requirement: Smart objects
World objects SHALL be able to advertise **affordances**: named capabilities with slots,
requirements, effects, an interaction location and approach direction, and an optional behaviour
fragment describing the interaction.

Agents SHALL query for an affordance (`Recharge`, `Cover`, `Seat`) rather than for an object class.

The system SHALL manage **slot reservation**, so two agents do not claim the same single-occupancy
slot, with reservations released on completion, failure, or agent destruction.

#### Scenario: Agent finds a way to recharge
- **WHEN** an agent needs energy
- **THEN** it SHALL query for the `Recharge` affordance and receive candidate objects with
  reachable interaction points, without knowing any object class

#### Scenario: Slot contention
- **WHEN** two agents target the same single-occupancy slot
- **THEN** exactly one SHALL reserve it and the other SHALL receive the next candidate

#### Scenario: Reservation released on failure
- **WHEN** an agent holding a reservation is destroyed or abandons the interaction
- **THEN** the slot SHALL be released

#### Scenario: New object type needs no AI change
- **WHEN** a new charging station type is added advertising `Recharge`
- **THEN** existing agents SHALL use it with no change to any AI graph

### Requirement: AI level of detail
Every agent SHALL be assigned an **LOD tier** determining both its think frequency and its
reasoning fidelity:

| Tier | Think rate | Perception | Navigation | Reasoning |
|---|---|---|---|---|
| `Full` | Up to every tick | All sensors, full queries | Individual pathfinding | Full graph |
| `Reduced` | 5–10 Hz | Cheap sensors, cached visibility | Flow field | Graph with costly nodes skipped |
| `Minimal` | 0.2–1 Hz | None; knowledge from shared channels | Macro movement | Coarse state only |
| `Statistical` | Aggregate | None | Group-level | Population model, not per agent |

Tier SHALL be derived from agent importance, distance to the nearest observer, visibility, and
gameplay-critical flags.

Tier transitions SHALL be hysteretic, and **promotion SHALL reconstruct plausible individual
state** so an agent entering `Full` does not visibly snap into a different behaviour.

Agents SHALL be pinnable to a minimum tier.

#### Scenario: 100,000 agents
- **WHEN** a simulation contains 100,000 agents of which 200 are near an observer
- **THEN** the near agents SHALL run at `Full` and the remainder at reduced tiers, with total cost
  bounded by tier budgets

#### Scenario: Promotion is not visible
- **WHEN** a `Statistical` agent is promoted as an observer approaches
- **THEN** individual state SHALL be reconstructed consistently with the population model, without
  a visible behavioural discontinuity

#### Scenario: Pinned agent
- **WHEN** a quest-critical NPC is pinned to `Full`
- **THEN** it SHALL never be demoted regardless of distance or budget pressure

### Requirement: AI determinism
AI SHALL be **deterministic**: given the same world state and inputs, agents SHALL make the same
decisions, so that network reconciliation, replay, and automated testing are valid.

Consequently:

- An agent's LOD tier and think schedule SHALL be a function of **simulation state** — distance,
  importance, tick number, and a stable agent ordering — and SHALL NOT depend on measured frame
  time or thread timing.
- Asynchronous work (path queries, environment queries, incremental planning) SHALL complete at a
  **deterministic tick** and be applied in a deterministic order, not when it happens to finish.
- Any randomness SHALL come from a seeded generator that is part of simulation state.
- Perception batching, sharing, and deferral SHALL be deterministic.

The AI budget controller SHALL operate in one of two declared modes:

| Mode | Behaviour |
|---|---|
| `Deterministic` | Thresholds are fixed configuration or replicated state. Budget overruns are **reported**, not corrected by varying the schedule. |
| `Adaptive` | Thresholds vary with measured load. Explicitly **not** replay-safe or lockstep-safe. |

The mode SHALL be a project-level declaration, and the documentation SHALL state plainly that
`Adaptive` forfeits deterministic replay and lockstep networking.

A determinism test mode SHALL hash AI state per tick so divergence is detectable and localisable.

#### Scenario: Re-simulation reproduces decisions
- **WHEN** ticks are re-simulated during network reconciliation
- **THEN** agents SHALL make identical decisions, because the schedule derives from simulation
  state rather than measured time

#### Scenario: Adaptive mode is honest about its cost
- **WHEN** a project selects `Adaptive` and then enables lockstep networking
- **THEN** the engine SHALL report the incompatibility at configuration time, not at desync time

#### Scenario: Async completion is deterministic
- **WHEN** a path query is issued on tick N
- **THEN** its result SHALL be applied on a deterministic tick, identically across runs, regardless
  of how long the query actually took

#### Scenario: Divergence is localisable
- **WHEN** the determinism test detects a hash mismatch
- **THEN** the tick, the agent, and the diverging state SHALL be reported

### Requirement: AI scheduling and budget
The AI scheduler SHALL distribute agent thinking across ticks so that per-tick cost is bounded,
using a deterministic rotation keyed on tick number and stable agent ordering within each tier.

Budgets SHALL be declared per tier and per subsystem (thinking, perception queries, planning,
environment queries), and utilisation SHALL be reported.

In `Deterministic` mode, exceeding a budget SHALL be reported and the work SHALL still be performed
across subsequent ticks in deterministic order. In `Adaptive` mode, thresholds MAY be adjusted.

Agents SHALL never be starved: the rotation SHALL guarantee that every agent thinks within a
bounded number of ticks for its tier.

#### Scenario: Load is spread across ticks
- **WHEN** 10,000 `Reduced` agents are due to think at 10 Hz
- **THEN** they SHALL be distributed across ticks by the rotation rather than all thinking on the
  same tick

#### Scenario: No starvation
- **WHEN** budget pressure persists
- **THEN** every agent SHALL still think within its tier's guaranteed maximum interval

#### Scenario: Budget reporting
- **WHEN** AI exceeds its declared budget
- **THEN** the overrun SHALL be reported with a breakdown by tier and subsystem

### Requirement: Navigation and locomotion integration
AI agents SHALL drive movement through the navigation system (see `navigation`) rather than
implementing their own pathfinding, and SHALL consume flow fields when their LOD tier specifies
it.

The AI graph SHALL be able to: request a path, follow a path with arrival and repath conditions,
request a flow-field destination, query reachability and path cost, and receive movement completion
and failure results.

Locomotion — translating desired velocity into animation and physics movement — SHALL remain the
responsibility of the character controller and animation system, not the AI graph.

#### Scenario: Tier selects the navigation strategy
- **WHEN** an agent is demoted to `Reduced`
- **THEN** it SHALL follow a shared flow field rather than computing an individual path

#### Scenario: Path failure is a graph result
- **WHEN** a requested path cannot be found
- **THEN** the graph node SHALL return failure so the behaviour can respond, rather than the agent
  stalling silently

### Requirement: Authoring
The editor SHALL provide an AI graph editor supporting: node-graph authoring across all four
reasoning models, a library of built-in nodes, user-defined nodes and subtrees, typed blackboard
declaration, response-curve editing for utility scoring, and GOAP action and goal definition.

Editing a graph SHALL trigger recompilation and live update of running agents, with agent state
reset where the program layout changed.

The editor SHALL surface: the compiled program size, per-node estimated cost, and blackboard
layout.

#### Scenario: Live iteration
- **WHEN** a designer edits a graph while the game runs
- **THEN** affected agents SHALL adopt the new program, with state reset only where the layout
  changed

#### Scenario: Reusable subtree
- **WHEN** a behaviour is factored into a subtree asset
- **THEN** it SHALL be usable across graphs, and edits SHALL propagate to all users

### Requirement: Gameplay API
AI SHALL be exposed to gameplay through components and a declarative authoring surface in Swift
and C++, with the scripting layer describing behaviour and the engine performing scheduling,
perception, and navigation natively.

Scripts SHALL be able to define: conditions, actions, utility considerations and scoring, GOAP
actions with preconditions and effects, and custom sensors and environment-query tests.

**AI acts through gameplay commands.** An agent that moves, attacks, builds, or interacts SHALL emit
the same commands a player emits (see `gameplay-framework`), and SHALL NOT call gameplay logic
directly. Consequently AI is exercised by the same validation as players, is recorded by replay, and
can be driven by a human or replaced by one without gameplay changes.

AI SHALL be able to **validate a command without issuing it**, so planning can ask whether an action
would be permitted and read the structured reason when it would not — the same validation the
interface and the authority use.

Script-defined nodes SHALL declare their data access so the scheduler can parallelise agent
thinking safely, exactly as for systems.

Script code SHALL NOT be invoked per agent per tick for agents whose graph is executing built-in
nodes only.

#### Scenario: Declarative action in Swift
- **WHEN** a developer defines a utility action with a scoring function in Swift
- **THEN** it SHALL be registered as a graph node, invoked only when that node is evaluated, and
  scheduled according to its declared access

#### Scenario: Native fast path
- **WHEN** an agent's think executes only built-in nodes
- **THEN** no script call SHALL occur for that agent that tick

#### Scenario: An agent is indistinguishable from a player
- **WHEN** an agent orders a unit to move
- **THEN** it SHALL emit the same command a player would, and the simulation SHALL not distinguish
  them

#### Scenario: Planning asks before acting
- **WHEN** an agent considers building a structure
- **THEN** it SHALL validate the command without issuing it and SHALL read the structured reason if
  it is not permitted

### Requirement: AI debugging
The engine SHALL provide an AI debugger, recognising that AI defects are typically about *why* an
agent decided something rather than about a crash.

It SHALL provide: per-agent inspection of active state, behaviour tree node, utility scores with
their contributing considerations, current plan and its remaining steps, blackboard values,
knowledge store contents with confidence, perception results, and current LOD tier and think
schedule.

It SHALL provide a **decision history**: a rolling record of an agent's decisions with the inputs
that produced them, inspectable after the fact.

It SHALL provide in-world visualisation: perception ranges and hits, knowledge entries with
confidence, navigation paths and flow fields, environment query candidates with their scores, and
smart-object reservations.

It SHALL support **record and replay** of a session's AI state for offline analysis.

#### Scenario: Why did it do that
- **WHEN** an agent makes an unexpected decision
- **THEN** the decision history SHALL show which action won, its score, the contributing
  considerations, and the knowledge state at the time

#### Scenario: Environment query visualisation
- **WHEN** a cover query returns a poor point
- **THEN** the visualisation SHALL show all candidates with per-test scores, revealing which test
  produced the ranking

#### Scenario: Offline analysis
- **WHEN** a rare misbehaviour is reproduced once
- **THEN** the recorded AI state SHALL be replayable for analysis without reproducing it live
