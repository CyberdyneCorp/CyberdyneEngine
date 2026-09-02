# Design: CyberAI and CyberML

## Context

Game AI has an unusual constraint profile. It drives gameplay, so it must be **deterministic** —
network reconciliation, replay, and automated testing all depend on it. It is also the subsystem
most tempted by adaptive scheduling, because thinking is expensive and most agents do not need to
think often. Those two pressures are in direct conflict, and most engines resolve it implicitly,
by being non-deterministic and not saying so.

This design resolves it explicitly, which is the most consequential decision here.

## Decisions

### 1. Agents are ECS entities

An agent is an entity with components: `AIAgent`, `AIState`, `Blackboard`, `PerceptionSensors`,
`Knowledge`, `NavAgent`. Behaviour runs as scheduled systems over queries, not as virtual
`Update()` on ten thousand controller objects.

**Rationale.** This is the workload ECS is actually good at: many entities sharing a component set,
processed in bulk, parallelised by declared access. It is the opposite of the UI case, where the
workload was hierarchical and ECS bought nothing.

**Consequence.** Agent "classes" are archetype templates plus a behaviour asset, not a type
hierarchy. Two agents running the same graph share the compiled program and differ only in data.

### 2. One graph, four reasoning models

A single **AI graph** asset composes hierarchical states, behaviour tree nodes, utility scoring,
and GOAP planning. A state may contain a behaviour tree; a behaviour tree node may be a utility
selector; a utility action may be a GOAP goal.

```
[STATE] Working
    │
[UTILITY] Choose task          Harvest 0.21 · Repair 0.54 · Recharge 0.92
    │
[GOAP] Recharge                → FindCharger → Navigate → Dock → Wait
```

**Rationale.** These models are complementary, not competing. State machines express mode;
behaviour trees express ordered fallback; utility expresses competing continuous needs; planning
expresses goals with preconditions. Forcing everything into one produces the familiar pathologies —
enormous behaviour trees encoding what utility scoring says in five lines, or state explosions
encoding what a planner derives.

**Alternative rejected — four separate asset types.** This is Unreal's position, arrived at
historically. It means four editors, four debuggers, four mental models, and awkward composition
at the boundaries.

**Trade-off accepted.** One graph format must express four semantics coherently, which makes the
compiler and the editor harder than any single model would be.

### 3. Graphs are compiled, not interpreted

A graph compiles to a compact program — a flat instruction stream with a side table of parameters —
shared by every agent using that asset. Agent state is a small per-entity block: current node,
stack, and blackboard.

**Rationale.** Ten thousand agents interpreting a node graph, chasing pointers per node per tick,
is the cost that caps agent counts in conventional implementations. A compiled program is
cache-friendly and shareable; per-agent cost becomes a program counter and a data block.

This mirrors the VFX decision, for the same reason.

### 4. AI is deterministic — and that constrains the budget controller

**AI is deterministic.** Given the same world state and the same inputs, agents make the same
decisions. This is required by `networking-and-replication` (re-simulation during reconciliation)
and `physics` (which AI drives).

This creates a direct tension with frame-budget adaptation. If which agents think this tick depends
on *measured frame time*, the simulation is no longer a function of its inputs, and replay and
reconciliation diverge.

**Resolution.** The AI schedule is a **deterministic function of simulation state**:

- An agent's LOD tier derives from distance, importance score, and other simulation state — never
  from measured time.
- Which agents think on a given tick derives from tier and a deterministic rotation keyed on tick
  number and a stable agent ordering.
- Async work (path queries, expensive planning) completes at a **deterministic tick**, not when it
  happens to finish. A query issued at tick N delivers at tick N+k for a fixed k, or is applied at
  a defined sync point in a deterministic order.

The budget controller therefore adjusts **thresholds**, not per-frame decisions, and does so in one
of two modes:

| Mode | Behaviour |
|---|---|
| `Deterministic` | Thresholds are fixed configuration or replicated state. Exceeding the budget is *reported*, not silently corrected. |
| `Adaptive` | Thresholds vary with measured load. Faster, and explicitly **not** replay-safe or lockstep-safe. |

**Rationale.** Both modes are legitimate — a single-player open world wants adaptive; a lockstep
RTS or a rollback netcode game needs deterministic. What is not legitimate is having one and
believing you have the other. Stating the modes makes the choice explicit at project setup rather
than discovered during multiplayer testing.

**This is the single most important requirement in the capability.**

### 5. Perception is scheduled globally, not per agent

Agents declare sensors; they do not issue queries. A perception scheduler runs per tick:

```
   10,000 agents with sensors
              │
   broad-phase candidate filtering        (spatial partition, faction, range)
              │
   cheap rejection                        (angle, distance², occlusion cache)
              │
   batched visibility queries             (grouped raycasts to the physics server)
              │
   knowledge updates
```

**Rationale.** The naive implementation — every agent raycasting every frame — is the dominant AI
cost in most games, and it is almost entirely redundant work: the same pairs, tested repeatedly,
with results that change slowly. Batching lets the engine sort by cost, share results between
agents, and spend the query budget where it matters.

**Trade-off accepted.** Perception results are tick-quantised and may be a tick stale. For game AI
this is imperceptible; for anything needing exact instantaneous visibility, a direct query remains
available and is documented as expensive.

### 6. Knowledge, not queries

Agents hold a knowledge store: perceived entities with last-known position, last-seen time, a
confidence that decays, and a threat assessment. Behaviour reads knowledge, not sensors.

**Rationale.** `canSeeEnemy` produces the characteristic bad AI behaviour of instantly forgetting a
target that steps behind a pillar. Decaying confidence gives search behaviour, suppressing fire at
a last-known position, and losing track — for free, as a property of the data model rather than as
special-case logic in every behaviour.

### 7. Smart objects invert the coupling

Objects advertise affordances (`Recharge`, `Cover`, `Seat`, `Repair`) with slots, requirements,
and effects. Agents query for an affordance, not for an object class.

**Rationale.** Without this, every AI must know every interactable class, and adding an object type
means editing AI. With it, adding a new charging station type is content.

### 8. AI LOD is a tier of *reasoning fidelity*, not just frequency

| Tier | Think rate | Perception | Navigation | Reasoning |
|---|---|---|---|---|
| `Full` | Every tick to 30 Hz | All sensors, full queries | Individual pathfinding | Full graph |
| `Reduced` | 5–10 Hz | Cheap sensors, cached visibility | Flow field | Graph with expensive nodes skipped |
| `Minimal` | 0.2–1 Hz | None; scripted knowledge | Macro movement | Coarse state only |
| `Statistical` | Aggregate | None | Group-level | Population model, not per agent |

**Rationale.** Reducing only frequency still pays full cost per think. Reducing fidelity as well is
what makes 100,000 agents feasible. The `Statistical` tier is the important one: distant crowds
become a population simulation, with individuals materialised on approach.

**Trade-off accepted.** Tier transitions must not produce visible behaviour discontinuities, which
requires state to be reconstructible when an agent is promoted. Specified as a requirement.

### 9. CyberML is separate, and firewalled

Neural inference is its own capability with its own backends. CyberAI may consume it through a
graph node; it does not depend on it.

**Determinism boundary.** Inference is not bit-reproducible across backends, devices, or driver
versions. Therefore ML output SHALL NOT drive authoritative gameplay unless the session is
explicitly **pinned** to a deterministic configuration (fixed backend, fixed precision, verified
reproducible), and that pinning is a declared property of the model asset.

**Rationale.** Same reasoning as the VFX firewall, arrived at from a different direction: without
it, an ML-driven decision inside a behaviour graph silently breaks reconciliation, and the failure
appears as unexplained multiplayer desync months later.

**Alternative rejected — building an inference runtime.** ONNX Runtime, Core ML, and TensorRT
represent enormous investment in operator coverage and per-device optimisation. Building one
competes with the entire engine for no differentiating benefit.

## Risks

- **The compiler and the unified graph semantics** are the hard parts. Mitigation: state trees and
  behaviour trees first; utility and GOAP are additive node kinds in the same IR.
- **Determinism is easy to lose accidentally.** Mitigation: the determinism test mode hashes AI
  state per tick, and CI runs it.
- **Perception batching correctness.** Shared and cached results can produce subtly wrong
  visibility. Mitigation: a direct-query path for validation, and tests comparing batched against
  naive results.
- **`Statistical` tier fidelity.** Population models drift from what individual simulation would
  have produced. Mitigation: promotion reconstructs plausible individual state, and the drift is
  documented as inherent.

## Open questions

- Whether the AI scheduler and the VFX budget controller should share one implementation. Likely
  yes; specified separately for now because their determinism requirements differ fundamentally.
- Whether GOAP planning should be incremental across ticks by default. Leaning yes for large
  plans, but it interacts with the deterministic completion rule and needs measurement.
- Whether `Statistical` tier agents should be entities at all, or a separate aggregate
  representation. Deferred; the tier interface is specified so either is possible.
