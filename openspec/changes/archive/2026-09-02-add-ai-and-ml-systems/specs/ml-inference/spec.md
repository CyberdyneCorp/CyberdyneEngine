## ADDED Requirements

### Requirement: Engine-owned inference abstraction
The engine SHALL provide **CyberML**: an abstraction for running trained machine-learning models,
with backends supplied by integrated inference runtimes.

The engine SHALL NOT implement a neural network runtime. Operator coverage and per-device
optimisation represent enormous investment with no differentiating benefit; the engine's
contribution is the asset model, the abstraction, the scheduling, and the determinism boundary.

Game and engine code SHALL depend only on the CyberML interface, never on a specific runtime's
types.

CyberML SHALL be removable at build time via `CY_ML`, and the engine SHALL be fully functional
without it.

#### Scenario: Backend types do not leak
- **WHEN** engine or game code is compiled
- **THEN** no inference runtime type SHALL appear outside its backend module

#### Scenario: ML disabled
- **WHEN** `CY_ML` is disabled
- **THEN** no inference runtime SHALL be fetched, built, or linked, and the rest of the engine
  SHALL be unaffected

#### Scenario: Training is out of scope
- **WHEN** a model is needed
- **THEN** it SHALL be trained externally and imported; the engine performs inference only

### Requirement: Model assets
A trained model SHALL be imported as a **model asset** carrying: the model payload, its input and
output tensor specifications (name, shape with optional dynamic dimensions, element type), the
backends it has been validated against, a declared precision, and a **determinism classification**
(see the determinism requirement).

Models SHALL be cooked per target platform, converted to the backend-native form where the backend
requires it, and content-addressed like any other asset.

Import SHALL validate the model and report unsupported operators for each target backend at cook
time, rather than at first inference.

#### Scenario: Unsupported operator
- **WHEN** a model uses an operator a target platform's backend does not support
- **THEN** cooking for that platform SHALL fail with the operator named, not fail at runtime

#### Scenario: Platform conversion
- **WHEN** a model is cooked for a platform whose backend uses a native format
- **THEN** the converted form SHALL be produced at cook time and shipped, not converted on load

### Requirement: Tensors and sessions
The engine SHALL provide a `Tensor` type — shape, element type, and a data view — and an
`InferenceSession` created from a model asset and a backend selection.

Sessions SHALL support: synchronous execution, asynchronous execution with deterministic
completion semantics as declared by the caller, batched execution of multiple inputs in one call,
and reuse across invocations without reallocation.

Tensor memory SHALL be allocatable from engine allocators, and SHALL support zero-copy where the
backend permits it.

#### Scenario: Batched inference
- **WHEN** many agents require the same model's inference in one tick
- **THEN** their inputs SHALL be batchable into one session call rather than one call per agent

#### Scenario: Session reuse
- **WHEN** a session runs every tick
- **THEN** it SHALL reuse its allocations rather than allocating per invocation

### Requirement: Backend abstraction
CyberML SHALL define an `InferenceBackend` interface, with the engine shipping backends for:
**ONNX Runtime** (portable default), **Core ML** (Apple platforms), **DirectML** (Windows), and
**TensorRT** (NVIDIA), each optional and capability-gated.

Backend selection SHALL be: explicit, or automatic by a declared preference order filtered by
availability and by the model's validated-backend list.

Backends SHALL report their capabilities — supported operators, precisions, device placement, and
whether execution is deterministic — and the engine SHALL surface these.

#### Scenario: Automatic selection
- **WHEN** a session is created without an explicit backend
- **THEN** the highest-preference available backend that the model declares as validated SHALL be
  chosen, and the choice SHALL be reported

#### Scenario: No backend available
- **WHEN** no configured backend can run a model on the current device
- **THEN** session creation SHALL fail with a diagnostic naming the reason, and the caller SHALL
  take its declared fallback

### Requirement: Inference determinism boundary
Inference SHALL be treated as **non-deterministic across backends, devices, and driver versions**
unless explicitly proven otherwise.

Model output SHALL NOT drive authoritative gameplay state — decisions affecting damage, hit
results, entity lifetime, physics, or replicated values — unless the session is **pinned**: a fixed
backend, fixed precision, and a configuration the model asset declares as verified reproducible.

Where a model is used inside an AI graph, the graph node SHALL declare whether its output is
authoritative. A non-pinned model feeding an authoritative node SHALL be rejected at cook time.

Development builds SHALL report attempts to use non-pinned inference output in a replicated or
physics-owned write.

#### Scenario: Multiplayer desync is prevented at cook time
- **WHEN** a non-pinned model's output feeds an authoritative AI decision
- **THEN** cooking SHALL fail with a diagnostic, rather than the mismatch surfacing later as a
  multiplayer desync

#### Scenario: Presentation use is unrestricted
- **WHEN** a model drives an animation blend weight or a non-authoritative visual choice
- **THEN** it SHALL be permitted without pinning

#### Scenario: Pinned model is verified
- **WHEN** a model is declared reproducible on a pinned configuration
- **THEN** CI SHALL verify that claim by comparing outputs across runs on that configuration

### Requirement: Scheduling and budget
Inference SHALL be schedulable on the job system or on a device queue, with a declared per-frame
budget covering invocation count and time.

Asynchronous inference SHALL never stall the frame; a result not yet available SHALL yield the
previous result or a declared default, with the staleness visible to the caller.

Inference exceeding its budget SHALL be deferred by priority and reported.

#### Scenario: Frame is not stalled
- **WHEN** an asynchronous inference has not completed
- **THEN** the caller SHALL receive the previous result or its declared default, flagged as stale,
  rather than the frame blocking

#### Scenario: Budget pressure
- **WHEN** more inference is requested than the budget allows
- **THEN** requests SHALL be deferred by priority and the deferral reported

### Requirement: AI graph integration
CyberML SHALL be usable from the AI graph as a node kind: inputs gathered from agent context,
knowledge, and world state; inference executed; and structured outputs written to the blackboard.

The node SHALL declare its model, its authoritativeness, its execution mode (synchronous,
asynchronous with staleness tolerance, or batched across agents), and its fallback behaviour when
inference is unavailable.

`ai-system` SHALL NOT depend on `ml-inference`: with `CY_ML` disabled, graphs containing inference
nodes SHALL fail to cook with a clear diagnostic, and graphs without them SHALL be unaffected.

#### Scenario: Classifier informs behaviour
- **WHEN** a perception classifier node produces a target classification with a confidence
- **THEN** it SHALL be written to the blackboard and usable by subsequent state, utility, or
  planning nodes

#### Scenario: Batched across agents
- **WHEN** many agents evaluate the same inference node in one tick
- **THEN** their inputs SHALL be batched into one session call

#### Scenario: AI without ML
- **WHEN** `CY_ML` is disabled
- **THEN** AI graphs without inference nodes SHALL cook and run unchanged

### Requirement: Gameplay API
CyberML SHALL be exposed to Swift and C++ with a small surface: load a model asset, create a
session, build input tensors, run, and read outputs — with typed helpers generated from the model
asset's declared tensor specifications where possible.

The API SHALL make the backend invisible to gameplay code.

#### Scenario: Backend-agnostic gameplay code
- **WHEN** gameplay runs a model
- **THEN** the same code SHALL work whether execution routes through Core ML, ONNX Runtime, or
  TensorRT

#### Scenario: Shape mismatch is caught early
- **WHEN** an input tensor's shape does not match the model's declared specification
- **THEN** the error SHALL be reported at session setup or first run with both shapes named

### Requirement: Inference diagnostics
The engine SHALL report per model: invocation count, wall-clock and device time per invocation,
batch sizes achieved, backend in use, memory consumed, staleness of asynchronous results, and
budget utilisation.

#### Scenario: Attributing cost
- **WHEN** inference is a significant frame cost
- **THEN** the report SHALL identify which models and which call sites account for it
