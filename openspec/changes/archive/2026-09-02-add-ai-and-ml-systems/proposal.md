# Add CyberAI and CyberML, and extend navigation for scale

## Why

The engine currently specifies navigation but no agent AI. Navigation moves an agent once
something has decided where it should go; nothing specifies the deciding.

Game AI is the third subsystem, after VFX and UI, where this engine can differentiate rather than
match. Unity gives building blocks without an architecture; Godot is deliberately minimal; Unreal
is the benchmark, with behaviour trees, state trees, EQS, perception, smart objects, and Mass for
scale — but arrived at as separate frameworks accumulated over time, which is visible in the
authoring experience.

The opening is scale and coherence. An RTS or simulation with tens of thousands of thinking agents
is exactly the workload that breaks per-agent virtual `Update()` architectures, and this engine
already has the ECS, job system, and budget-controller patterns that such a workload needs. A
single composable graph — states, behaviour trees, utility scoring, and planning in one asset —
is a better authoring story than four separate asset types.

Neural inference is a separate concern that is frequently conflated with it. It is specified as
its own capability so that game AI does not acquire a dependency on an ML runtime, and so the
determinism consequences of inference are contained.

## What Changes

- **New `ai-system` capability (CyberAI).** Agents are ECS entities, never heavyweight AI objects.
  A **unified AI graph** composes hierarchical state trees, behaviour trees, utility scoring, and
  GOAP planning in one asset, **compiled** to a compact program shared across all agents using it.
- **Batched, engine-scheduled perception.** Sensors do not each issue their own queries; a
  perception scheduler filters candidates and batches spatial and visibility queries.
- **Knowledge with decay.** Agents remember what they perceived, with confidence decaying over
  time, rather than answering `canSeeEnemy` per frame.
- **Environment queries** for spatial reasoning (cover, flanking, placement).
- **Smart objects**: world objects advertise capabilities, so agents query for an affordance
  rather than knowing object classes.
- **AI LOD and an AI frame budget**, with think frequency and reasoning fidelity tiered by
  importance and distance.
- **A determinism contract**, and the tension it creates with budget-driven scheduling, stated
  explicitly and resolved: AI is deterministic, so the schedule is a function of simulation state,
  never of measured frame time, in deterministic mode.
- **New `ml-inference` capability (CyberML).** A backend abstraction for running trained models —
  ONNX Runtime, Core ML, DirectML, TensorRT — with model assets, tensors, sessions, async
  execution, and a budget. Firewalled from authoritative gameplay unless explicitly pinned.
- **`navigation` extended for scale**: flow fields for many agents sharing a destination,
  hierarchical pathfinding for large worlds, navigation volumes for 3D movement, streaming, and
  crowd simulation as a scheduled system.

Non-goals: training neural networks in-engine, LLM agent frameworks beyond an inference node,
dialogue systems, and animation-driven locomotion (which belongs to `animation-and-skinning`).

## Capabilities

### New Capabilities

- `ai-system` — agent model, the unified graph and its compiler, perception, knowledge,
  environment queries, smart objects, AI LOD, budget and scheduling, determinism, authoring,
  Swift API, and debugging.
- `ml-inference` — model assets, tensors, sessions, backend abstraction, async execution,
  budgeting, determinism boundary, and the AI graph integration point.

### Modified Capabilities

- `navigation` — add flow fields, hierarchical pathfinding, navigation volumes, streaming, and
  crowd simulation; extend pathfinding with deterministic async completion.
- `thirdparty-dependencies` — record CyberAI as engine-built; record the inference runtimes as
  integrated, optional backends.
- `build-system-and-platforms` — add `CY_AI` and `CY_ML`.

## Impact

- **Dependencies**: adds optional inference runtimes (ONNX Runtime, and platform runtimes where
  available), all gated behind `CY_ML`. Recast/Detour remains the navmesh generation dependency.
  No new required dependency.
- **ECS**: adds agent components and AI systems to the schedule; the AI scheduler becomes a
  consumer of the job system alongside physics and rendering.
- **Physics**: perception batching becomes a significant consumer of spatial and raycast queries;
  the batching interface is specified in `ai-system` and consumes the existing query API.
- **Determinism**: this is the first subsystem that both must be deterministic *and* wants
  frame-budget adaptation. The resolution is specified rather than left implicit.
- **Risk**: the graph compiler and the AI scheduler are where risk concentrates, as with VFX.
