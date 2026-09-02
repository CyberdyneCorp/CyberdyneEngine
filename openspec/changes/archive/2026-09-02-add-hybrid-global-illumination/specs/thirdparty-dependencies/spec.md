## MODIFIED Requirements

### Requirement: What the engine builds itself
The engine SHALL implement, rather than integrate:

- the **ECS core**, scheduler, and job system — the performance model is the engine's identity
- the **renderer**: RHI, render graph, GPU scene, culling, pipelines, lighting, GI, and
  post-processing
- the **material compiler**: the authoring model, the material IR and its optimisation passes, the
  closure model and its lowering to shading models, parameter classification, quality tier
  generation, cost analysis, material classification and binning, and the GPU material table —
  this is where material cost is actually decided, and it is inseparable from the GPU scene and
  the renderer's budget model. The engine does **not** implement a shader optimiser or backend
  code generator; it produces good input to somebody else's.
- the **renderer budget arbiter**, the temporal framework, and the **denoising framework** — frame
  cost allocation, history management, and edge-aware reconstruction are cross-subsystem policy
  that no library can hold. A vendor or machine-learning denoiser MAY be integrated as a backend
  behind the engine's interface.
- the **illumination architecture**: the GI scene and its representations, the surface cache, the
  radiance cache and its scheduler, the tracing tiers and their selection, the confidence model,
  the resolve, and the illumination budget policy — hybrid GI is a scheduling problem over the
  engine's own scene, streaming, geometry hierarchy, and budget, and a library that owned any of
  those would own the engine
- the **virtual geometry system**: cluster hierarchy and its error metric, page format, GPU
  traversal and culling, the geometry cache and residency manager, the visibility buffer and
  material resolve, and its budget controller — the architecture is native to the GPU scene,
  streaming, and budgeting systems, which no external library could be
- the **VFX system**: asset model, graph compiler, particle storage, GPU simulation, event
  routing, scalability policy, and renderers — one of the few areas where an engine can still
  differentiate, and one that must be co-designed with the renderer and the frame budget
- the **AI system**: agent model, the unified behaviour graph and its compiler, perception
  scheduling and batching, the knowledge model, environment queries, smart objects, AI LOD, and
  the deterministic scheduler — the scale and determinism policy are engine concerns, and no
  third-party framework provides them
- the **animation runtime**: asset model, graph and rig compilers, pose evaluation and storage,
  the GPU pose world, animation LOD and pose sharing, motion matching, the constraint framework,
  and retargeting — animation couples to the renderer, physics, AI, and VFX, and its scale
  behaviour is engine policy
- the **UI system**: element storage, layout, styling, input routing, animation, and rendering
- the **scene, prefab, and serialization** model
- the **asset pipeline** and package format
- the **AudioServer**, bus graph, voice management, and the audio importance and tiering policy —
  audio scheduling and budget policy are engine concerns that interact with the job system and ECS
- the **networking and replication** model: replication schemas and encoding, snapshot and delta,
  interest management and its priority scheduler, network LOD and bandwidth budgeting, prediction,
  rollback, lag compensation, session management, and the network profiler — transports and crypto
  are integrated beneath it
- the **C ABI and Swift binding** layer
- the **editor**

These are where engine-level decisions compound, and where a general-purpose library would impose
its own architecture.

Shader toolchains (Slang, SPIR-V tools, DXC where needed, platform shader compilers), GPU vendor
upscalers, and GPU capture tools SHALL be integrated rather than built, behind engine-owned
interfaces that expose no vendor types.

Algorithms with published references — noise functions, sorting networks, curl fields,
pathfinding heuristics — MAY be implemented in engine code from those references. Implementing a
published algorithm is not a dependency and does not require a manifest entry.

#### Scenario: Rejecting a general-purpose ECS library
- **WHEN** an existing ECS library is proposed
- **THEN** it SHALL be evaluated against the requirement that the scheduler, change detection, and
  storage layout are co-designed with the renderer and the job system, which a general-purpose
  library cannot be

#### Scenario: Rejecting middleware as the audio foundation
- **WHEN** a full audio middleware engine is proposed as the foundation
- **THEN** it SHALL be evaluated against the requirement that voice budgets, tiering, job system
  integration, and asset residency are engine policy, which a middleware engine owning its own
  threading and asset model cannot provide

#### Scenario: Rejecting a third-party VFX runtime
- **WHEN** an external VFX runtime is proposed
- **THEN** it SHALL be evaluated against the requirement that the VFX system publishes into the
  engine's GPU scene, compiles through the engine's shader pipeline, and is governed by the
  engine's frame budget controller — coupling a third-party runtime cannot provide

#### Scenario: Integrating within an owned subsystem
- **WHEN** a proven library solves a bounded problem inside an owned subsystem — navmesh
  generation within navigation, or an inference runtime within CyberML
- **THEN** it SHALL be integrated behind the subsystem's own interface, since owning the
  architecture does not require implementing every algorithm

#### Scenario: The material IR is owned; the shader backend is not
- **WHEN** the shader toolchain is upgraded or replaced
- **THEN** the material IR, its optimisation passes, and the cost model SHALL be unaffected, since
  the engine owns the material compiler and integrates the shader compiler

#### Scenario: Rejecting a drop-in GI solution
- **WHEN** an external global illumination library is proposed
- **THEN** it SHALL be evaluated against the requirement that illumination reads the engine's GPU
  scene, consumes the virtual geometry hierarchy at its own error target, fills a surface cache
  from engine-compiled secondary material programs, and holds an allocation from the engine's
  budget arbiter — coupling a self-contained solution cannot provide
