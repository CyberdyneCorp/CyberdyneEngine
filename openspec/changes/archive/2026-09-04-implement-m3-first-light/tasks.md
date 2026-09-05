# Tasks: M3 — First light

Ordered. The graph's scheduling model is spiked first because every pass depends on the answer.
Then M2's carried-forward debts, because M3 is what makes them expensive. Then the RHI, then the
frame, then the artefact.

## 0. Spike — render graph scheduling under async compute

M3's named risk (`design.md` §2). Its only deliverable is a decision.

- [x] 0.1 Prototype barrier and aliasing derivation across a hard case: a compute pass writing a
      resource a graphics pass samples, while a second compute pass writes a different subresource
      of the same image, on a separate queue
- [x] 0.2 Confirm the model produces correct semaphores across queues, not just pipeline barriers
      within one
- [x] 0.3 Measure whether transient aliasing actually reduces peak GPU memory, against a
      no-aliasing build
- [x] 0.4 If barriers cannot be derived correctly under async compute, **STOP** and propose the
      roadmap change before section 2 proceeds. Explicit synchronisation in every pass is the
      outcome this milestone exists to prevent; it is not something to absorb quietly.

## 1. Carried forward from M2

Seven debts the M2 gate recorded. Do them first: M3 is the milestone that makes each one expensive.

- [x] 1.1 **The `four-profiles` flake** — ~1 failure in 30 under Debug, load-induced rather than a
      property of any case. A pull request is exposed to it three times over once M2's gate joins.
      The honest fix is a per-case wall-clock budget at `-O0` under many back-to-back suites, in the
      harness or the taxonomy — not in any individual test. **Do this before M3 adds suites.**
- [x] 1.2 **The state hash covers 4 of 17 subjects.** The ECS's Parent/Children and all twelve scene
      built-ins are registered by name with no `reflect::TypeInfo`, so a divergence in a node's name,
      parent, sibling order or visibility does not change the hash. Close it, and **reflect M3's
      renderer components as they are written** — a component added unreflected is a component M9
      cannot see.
- [x] 1.3 **The determinism firewall guards zero fields.** `Classified<>` is correct and its
      crossings do not compile, but nothing has adopted it. Adopt it as components are written —
      cheapest at the moment a component is authored, and every unwrapped field is one M9's lint
      inherits unguarded.
- [x] 1.4 **Hot reload is at zero** in `core-assets-and-io` — no watcher, no `reload` entry point.
      M3's shader and material iteration is the first thing that actually wants it.
- [x] 1.5 **`just test-sanitize --tests .` is unusable**: the recipe exports
      `CY_TEST_BUDGET_SCALE=0` and `test_assertions.cpp` asserts `budget_scale() > 0.0`. Fix it, and
      add the `schedule:` trigger `testing-and-quality` requires for the nightly sanitizer run.
- [x] 1.6 **No ECS benchmark runner.** `cy_add_benchmark(NAME ecs ...)`; `m2.toml` already names it.
- [x] 1.7 **Two sequence-numbers-where-identities-belong**: the stage scheduler's tie-break and
      `CommandBuffer`'s merge key are both registration order. Fine with no plugins and no
      conditional registration; not fine afterwards. `StateProviderRegistry::finalize()` shows the
      right shape.

## 2. The RHI and the render graph — `rhi-and-render-graph` → Working

### 2.1 The null backend, first

- [x] 2.1.1 The explicit RHI interface: devices, queues, command buffers, resources, pipelines
- [x] 2.1.2 **The null backend** (`design.md` §1) — written before Vulkan so the RHI is an interface
      rather than a wrapper over whichever Vulkan calls were convenient
- [x] 2.1.3 The backend capability model: capabilities are queried, never assumed

### 2.2 The graph — **invariant, M3**

- [x] 2.2.1 Passes declare reads and writes; **there is no API to emit a barrier**
- [x] 2.2.2 Barriers, transient aliasing and pass scheduling derived from the declarations
- [x] 2.2.3 Async compute, per the spike's outcome
- [x] 2.2.4 **The structural gate**: barrier-emitting calls live behind an interface only the graph
      can reach, and the build fails if one appears outside it. Prove it by introducing one.
- [x] 2.2.5 Parallel command recording
- [x] 2.2.6 Resource lifetime and frames in flight
- [x] 2.2.7 Descriptor management; memory management
- [x] 2.2.8 Validation and debugging; graph visualisation

### 2.3 Vulkan

- [x] 2.3.1 Vulkan behind the RHI, via volk and VMA — no Vulkan type above `src/backends/`
- [x] 2.3.2 Surface creation through M0's `DisplayServer` seam, which has waited three milestones
- [x] 2.3.3 Validation layers wired in development builds

## 3. Shaders — `shader-system` → Working

- [x] 3.1 Slang as the authoring language; SPIR-V as the interchange form
- [x] 3.2 The compilation pipeline; permutations and specialization
- [x] 3.3 **Reflection-driven binding** — bindings derive from shader reflection, not from a
      hand-maintained table
- [x] 3.4 The shader library and its tiered cache
- [x] 3.5 Hot reload, over task 1.4's watcher
- [x] 3.6 Global shader parameters; compute and utility shaders
- [x] 3.7 Shader diagnostics; pipeline state object management
- [x] 3.8 The seam for engine-generated source (M7's material compiler) — same pipeline, same cache,
      no second toolchain

## 4. The render server, the GPU scene, and the frame

### 4.1 `rendering-architecture` → Working

- [x] 4.1.1 The handle-based render server; scene, view and instance model
- [x] 4.1.2 The simulation-to-render snapshot, taken at M2's commit boundary
- [x] 4.1.3 Frame structure; render targets and formats
- [x] 4.1.4 **The GPU scene as a publication interface** (`design.md` §4), designed for the
      producers that arrive at M7 rather than for the one producer that exists now
- [x] 4.1.5 **Deterministic submission order** (`design.md` §6) — sort keys from stable inputs,
      never from iteration order, pointer values or publication order
- [x] 4.1.6 Debug visualisation; render statistics

### 4.2 Geometry and materials → Working

- [x] 4.2.1 Mesh representation; vertex compression; instancing
- [x] 4.2.2 Texture formats and compression
- [x] 4.2.3 The core BRDF; shading models; image-based lighting
- [x] 4.2.4 The material model, parameter storage, and the standard material
- [x] 4.2.5 Material validation and fallback materials

### 4.3 The frame — `rendering-forward-clustered` → Working

- [x] 4.3.1 The cluster grid; light and volume assignment
- [x] 4.3.2 Depth prepass; draw sorting; instance data
- [x] 4.3.3 Pass order; shading model dispatch
- [x] 4.3.4 Pipeline diagnostics

### 4.4 Lights, shadows and culling → Seed

- [x] 4.4.1 Light types with **physical units**
- [x] 4.4.2 The shadow atlas; directional cascades; shadow filtering
- [x] 4.4.3 Spatial indexing; frustum culling; LOD selection

## 5. Conventions on a GPU — `core-math` → Complete

- [x] 5.1 **Reversed-Z asserted from the device** (`design.md` §3): `[0,1]`, cleared to 0, compared
      GreaterEqual, near→1 and far→0 as sampled back. A golden image is not sufficient evidence.
- [x] 5.2 **Camera-relative rendering with the first draw**, not when precision breaks
- [x] 5.3 A scene one million units from the origin renders without visible jitter
- [x] 5.4 The handedness and axis conventions verified against rendered output

## 6. The artefact

- [x] 6.1 `samples/03-first-light` — a lit, textured, shadowed scene with a moving camera
- [x] 6.2 `just run-sample first-light`
- [x] 6.3 **Golden-image tests** in `tests/render/`, which has waited since M0 with a README naming
      this milestone
- [x] 6.4 The same frame through the **null backend** in CI, with no GPU
- [x] 6.5 The XR prerequisite checks wired as tests: multi-view capable, runtime-driven frame
      timing, late-latch seam — `tests/render/README.md` has recorded them since M0

## 7. Closing the milestone

- [x] 7.1 Golden images pass on Vulkan; the null backend records the same graph
- [x] 7.2 No barrier call exists outside the render graph, proven by introducing one
- [x] 7.3 Transient aliasing measurably reduces peak GPU memory against a no-aliasing build
- [x] 7.4 Shader hot reload replaces a material's shader without a restart
- [x] 7.5 A scene one million units from the origin renders without precision loss
- [x] 7.6 Frame submission order is identical across runs
- [x] 7.7 The XR prerequisite checks pass
- [x] 7.8 All four profiles build clean and `just test-all` is green in each
- [x] 7.9 Sanitizers green over the new suites; the nightly schedule exists
- [x] 7.10 `just roadmap-milestone m3` exits zero, and m2, m1 and m0 still do
- [x] 7.11 Update `status.yaml` and `capability-matrix.md`; record what is thinner than the tasks claim
- [x] 7.12 `openspec validate --specs --strict` passes; archive this change
- [x] 7.13 Open the M4 change — hot reload across the ABI with live Swift objects is its named spike
