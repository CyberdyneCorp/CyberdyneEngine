# Tasks: CyberAI and CyberML

Specification-stage change. Sections 1 and 2 are the work of this change and are complete; the
change is archived on that basis.

Sections 3 to 8 record the implementation the decision implies and are **deliberately deferred to
implementation changes**. The graph compiler (section 4) and the deterministic scheduler
(section 3.2) are where the risk concentrates; see `design.md`.

## 1. Specification

- [x] 1.1 Record rationale, the determinism resolution, and rejected alternatives in `design.md`
- [x] 1.2 New `ai-system`: ECS-native agents, unified graph across state trees, behaviour trees,
      utility and GOAP, compiled programs, blackboard, batched perception, knowledge with decay,
      environment queries, smart objects, AI LOD, determinism contract, scheduling and budget,
      navigation integration, authoring, gameplay API, debugging
- [x] 1.3 New `ml-inference`: model assets, tensors and sessions, backend abstraction, determinism
      boundary, scheduling, AI graph integration, gameplay API, diagnostics
- [x] 1.4 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 `navigation` — add flow fields, hierarchical pathfinding, navigation volumes, streaming,
      and crowd simulation; require deterministic async path completion
- [x] 2.2 `thirdparty-dependencies` — record the AI system as engine-built; add Recast/Detour
      framing, ONNX Runtime and the platform inference runtimes as optional; add the scenario that
      integrating within an owned subsystem is consistent with owning the architecture
- [x] 2.3 `build-system-and-platforms` — add `CY_AI` and `CY_ML`, and require features to declare
      dependencies on other features
- [x] 2.4 `ecs-core` — reviewed; no requirement change needed. AI agents are ordinary entities and
      AI systems are ordinary systems; the determinism they require is already provided by the
      deterministic scheduling mode.
- [x] 2.5 `physics` — reviewed; no requirement change needed. Perception batching consumes the
      existing thread-safe query API; the batching policy lives in `ai-system`.
- [x] 2.6 `networking-and-replication` — reviewed; no requirement change needed. The AI
      determinism requirement is what protects reconciliation, and it lives with AI.

## 3. AI runtime (deferred to implementation)

- [ ] 3.1 Agent components, archetype templates, and per-agent state blocks
- [ ] 3.2 Deterministic scheduler: tier assignment from simulation state, tick-keyed rotation,
      starvation guarantees, and the two budget modes
- [ ] 3.3 Blackboard with compile-time key resolution
- [ ] 3.4 Behaviour tree execution with resumption and abort semantics
- [ ] 3.5 Hierarchical state tree execution
- [ ] 3.6 Utility scoring with response curves and hysteresis
- [ ] 3.7 GOAP planner with incremental planning and deterministic search
- [ ] 3.8 AI LOD tiers, including `Statistical` and promotion state reconstruction

## 4. AI graph compiler (deferred to implementation)

- [ ] 4.1 Graph IR spanning all four reasoning models
- [ ] 4.2 Constant folding, dead-branch elimination, condition hoisting
- [ ] 4.3 Program emission and the shared-program runtime format
- [ ] 4.4 Error reporting mapped to graph nodes and pins
- [ ] 4.5 Cook-time integration and content addressing

## 5. Perception and knowledge (deferred to implementation)

- [ ] 5.1 Sensor components and the perception scheduler
- [ ] 5.2 Broad-phase filtering, cheap rejection, occlusion caching
- [ ] 5.3 Batched query submission to the physics server and result sharing
- [ ] 5.4 Knowledge store with confidence decay, eviction, and sharing channels
- [ ] 5.5 Environment query generators, tests, scoring, and async completion

## 6. World interaction (deferred to implementation)

- [ ] 6.1 Smart object affordances, slots, and reservation with release on failure
- [ ] 6.2 Navigation integration: path requests, flow-field following, reachability queries
- [ ] 6.3 Crowd system integrating avoidance, separation, formations, and priority

## 7. CyberML (deferred to implementation)

- [ ] 7.1 Model asset import, validation, per-platform cooking, determinism classification
- [ ] 7.2 Tensor and session API with zero-copy where available
- [ ] 7.3 ONNX Runtime backend
- [ ] 7.4 Core ML, DirectML, TensorRT backends
- [ ] 7.5 Async scheduling, staleness reporting, budget enforcement
- [ ] 7.6 AI graph inference node with batching across agents
- [ ] 7.7 Cook-time rejection of non-pinned models feeding authoritative decisions

## 8. Tooling and validation (deferred to implementation)

- [ ] 8.1 AI graph editor across all four models, with response-curve and GOAP editing
- [ ] 8.2 AI debugger: per-agent inspection, decision history, in-world visualisation
- [ ] 8.3 Record and replay of AI state
- [ ] 8.4 Determinism tests: per-tick AI state hashing, divergence localisation
- [ ] 8.5 Perception correctness tests: batched results compared against naive queries
- [ ] 8.6 Planner tests: determinism, budget termination, replanning on invalidation
- [ ] 8.7 Benchmarks: 1k / 10k / 100k agents across tier mixes, as regression guards
- [ ] 8.8 CI verification that models declared reproducible actually are on their pinned config
