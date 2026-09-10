## MODIFIED Requirements

### Requirement: Backend abstraction
CyberML SHALL define an `InferenceBackend` interface, with the engine shipping backends for:
**ONNX Runtime** (portable default), **Core ML** (Apple platforms), **DirectML** (Windows), and
**TensorRT** (NVIDIA), each optional and capability-gated.

**ONNX Runtime SHALL be the reference backend**, and it is the one an interface without an
implementation is measured against. It is the portable default because it is the only one of the
four that runs on every target the engine ships to, and because its model format is the interchange
the other three import from — so a model validated against it is a model the others can be asked for.
A capability whose only backend is an interface has not been exercised, and `ml-inference` at Seed
means this backend exists and runs a real model rather than that the abstraction compiles.

`CY_ML_ONNXRUNTIME` SHALL gate it, ONNX Runtime SHALL be declared in the dependency manifest with a
licence identifier and a justification like every other integrated library, and the engine SHALL
build and pass its suites with the option OFF as well as ON.

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

#### Scenario: The reference backend runs a real model
- **WHEN** `ml-inference` is claimed at Seed or above
- **THEN** the ONNX Runtime backend SHALL load a committed model asset, run an inference, and
  produce a checked result — the abstraction compiling is not the claim

#### Scenario: The reference backend is removable
- **WHEN** `CY_ML_ONNXRUNTIME` is disabled
- **THEN** ONNX Runtime SHALL NOT be fetched, built or linked, the engine SHALL build and pass its
  suites, and a session requesting it SHALL fail with a diagnostic naming the option
