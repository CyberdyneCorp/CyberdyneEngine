# Tasks: M3 — First light

Ordered. The graph's scheduling model is spiked first because every pass depends on the answer.
Then M2's carried-forward debts, because M3 is what makes them expensive. Then the RHI, then the
frame, then the artefact.

## 0. Spike — render graph scheduling under async compute

M3's named risk (`design.md` §2). Its only deliverable is a decision.

- [ ] 0.1 Prototype barrier and aliasing derivation across a hard case: a compute pass writing a
      resource a graphics pass samples, while a second compute pass writes a different subresource
      of the same image, on a separate queue
- [ ] 0.2 Confirm the model produces correct semaphores across queues, not just pipeline barriers
      within one
- [ ] 0.3 Measure whether transient aliasing actually reduces peak GPU memory, against a
      no-aliasing build
- [ ] 0.4 If barriers cannot be derived correctly under async compute, **STOP** and propose the
      roadmap change before section 2 proceeds. Explicit synchronisation in every pass is the
      outcome this milestone exists to prevent; it is not something to absorb quietly.

## 1. Carried forward from M2

Seven debts the M2 gate recorded. Do them first: M3 is the milestone that makes each one expensive.

- [ ] 1.1 **The `four-profiles` flake** — ~1 failure in 30 under Debug, load-induced rather than a
      property of any case. A pull request is exposed to it three times over once M2's gate joins.
      The honest fix is a per-case wall-clock budget at `-O0` under many back-to-back suites, in the
      harness or the taxonomy — not in any individual test. **Do this before M3 adds suites.**
- [ ] 1.2 **The state hash covers 4 of 17 subjects.** The ECS's Parent/Children and all twelve scene
      built-ins are registered by name with no `reflect::TypeInfo`, so a divergence in a node's name,
      parent, sibling order or visibility does not change the hash. Close it, and **reflect M3's
      renderer components as they are written** — a component added unreflected is a component M9
      cannot see.
- [ ] 1.3 **The determinism firewall guards zero fields.** `Classified<>` is correct and its
      crossings do not compile, but nothing has adopted it. Adopt it as components are written —
      cheapest at the moment a component is authored, and every unwrapped field is one M9's lint
      inherits unguarded.
- [ ] 1.4 **Hot reload is at zero** in `core-assets-and-io` — no watcher, no `reload` entry point.
      M3's shader and material iteration is the first thing that actually wants it.
- [ ] 1.5 **`just test-sanitize --tests .` is unusable**: the recipe exports
      `CY_TEST_BUDGET_SCALE=0` and `test_assertions.cpp` asserts `budget_scale() > 0.0`. Fix it, and
      add the `schedule:` trigger `testing-and-quality` requires for the nightly sanitizer run.
- [ ] 1.6 **No ECS benchmark runner.** `cy_add_benchmark(NAME ecs ...)`; `m2.toml` already names it.
- [ ] 1.7 **Two sequence-numbers-where-identities-belong**: the stage scheduler's tie-break and
      `CommandBuffer`'s merge key are both registration order. Fine with no plugins and no
      conditional registration; not fine afterwards. `StateProviderRegistry::finalize()` shows the
      right shape.

## 2. The RHI and the render graph — `rhi-and-render-graph` → Working

### 2.1 The null backend, first

- [ ] 2.1.1 The explicit RHI interface: devices, queues, command buffers, resources, pipelines
- [ ] 2.1.2 **The null backend** (`design.md` §1) — written before Vulkan so the RHI is an interface
      rather than a wrapper over whichever Vulkan calls were convenient
- [ ] 2.1.3 The backend capability model: capabilities are queried, never assumed

### 2.2 The graph — **invariant, M3**

- [ ] 2.2.1 Passes declare reads and writes; **there is no API to emit a barrier**
- [ ] 2.2.2 Barriers, transient aliasing and pass scheduling derived from the declarations
- [ ] 2.2.3 Async compute, per the spike's outcome
- [ ] 2.2.4 **The structural gate**: barrier-emitting calls live behind an interface only the graph
      can reach, and the build fails if one appears outside it. Prove it by introducing one.
- [ ] 2.2.5 Parallel command recording
- [ ] 2.2.6 Resource lifetime and frames in flight
- [ ] 2.2.7 Descriptor management; memory management
- [ ] 2.2.8 Validation and debugging; graph visualisation

### 2.3 Vulkan

- [ ] 2.3.1 Vulkan behind the RHI, via volk and VMA — no Vulkan type above `src/backends/`
- [ ] 2.3.2 Surface creation through M0's `DisplayServer` seam, which has waited three milestones
- [ ] 2.3.3 Validation layers wired in development builds

## 3. Shaders — `shader-system` → Working

- [ ] 3.1 Slang as the authoring language; SPIR-V as the interchange form
- [ ] 3.2 The compilation pipeline; permutations and specialization
- [ ] 3.3 **Reflection-driven binding** — bindings derive from shader reflection, not from a
      hand-maintained table
- [ ] 3.4 The shader library and its tiered cache
- [ ] 3.5 Hot reload, over task 1.4's watcher
- [ ] 3.6 Global shader parameters; compute and utility shaders
- [ ] 3.7 Shader diagnostics; pipeline state object management
- [ ] 3.8 The seam for engine-generated source (M7's material compiler) — same pipeline, same cache,
      no second toolchain

## 4. The render server, the GPU scene, and the frame

### 4.1 `rendering-architecture` → Working

- [ ] 4.1.1 The handle-based render server; scene, view and instance model
- [ ] 4.1.2 The simulation-to-render snapshot, taken at M2's commit boundary
- [ ] 4.1.3 Frame structure; render targets and formats
- [ ] 4.1.4 **The GPU scene as a publication interface** (`design.md` §4), designed for the
      producers that arrive at M7 rather than for the one producer that exists now
- [ ] 4.1.5 **Deterministic submission order** (`design.md` §6) — sort keys from stable inputs,
      never from iteration order, pointer values or publication order
- [ ] 4.1.6 Debug visualisation; render statistics

### 4.2 Geometry and materials → Working

- [ ] 4.2.1 Mesh representation; vertex compression; instancing
- [ ] 4.2.2 Texture formats and compression
- [ ] 4.2.3 The core BRDF; shading models; image-based lighting
- [ ] 4.2.4 The material model, parameter storage, and the standard material
- [ ] 4.2.5 Material validation and fallback materials

### 4.3 The frame — `rendering-forward-clustered` → Working

- [ ] 4.3.1 The cluster grid; light and volume assignment
- [ ] 4.3.2 Depth prepass; draw sorting; instance data
- [ ] 4.3.3 Pass order; shading model dispatch
- [ ] 4.3.4 Pipeline diagnostics

### 4.4 Lights, shadows and culling → Seed

- [ ] 4.4.1 Light types with **physical units**
- [ ] 4.4.2 The shadow atlas; directional cascades; shadow filtering
- [ ] 4.4.3 Spatial indexing; frustum culling; LOD selection

## 5. Conventions on a GPU — `core-math` → Complete

- [ ] 5.1 **Reversed-Z asserted from the device** (`design.md` §3): `[0,1]`, cleared to 0, compared
      GreaterEqual, near→1 and far→0 as sampled back. A golden image is not sufficient evidence.
- [ ] 5.2 **Camera-relative rendering with the first draw**, not when precision breaks
- [ ] 5.3 A scene one million units from the origin renders without visible jitter
- [ ] 5.4 The handedness and axis conventions verified against rendered output

## 6. The artefact

- [ ] 6.1 `samples/03-first-light` — a lit, textured, shadowed scene with a moving camera
- [ ] 6.2 `just run-sample first-light`
- [ ] 6.3 **Golden-image tests** in `tests/render/`, which has waited since M0 with a README naming
      this milestone
- [ ] 6.4 The same frame through the **null backend** in CI, with no GPU
- [ ] 6.5 The XR prerequisite checks wired as tests: multi-view capable, runtime-driven frame
      timing, late-latch seam — `tests/render/README.md` has recorded them since M0

## 7. Closing the milestone

- [ ] 7.1 Golden images pass on Vulkan; the null backend records the same graph
- [ ] 7.2 No barrier call exists outside the render graph, proven by introducing one
- [ ] 7.3 Transient aliasing measurably reduces peak GPU memory against a no-aliasing build
- [ ] 7.4 Shader hot reload replaces a material's shader without a restart
- [ ] 7.5 A scene one million units from the origin renders without precision loss
- [ ] 7.6 Frame submission order is identical across runs
- [ ] 7.7 The XR prerequisite checks pass
- [ ] 7.8 All four profiles build clean and `just test-all` is green in each
- [ ] 7.9 Sanitizers green over the new suites; the nightly schedule exists
- [ ] 7.10 `just roadmap-milestone m3` exits zero, and m2, m1 and m0 still do
- [ ] 7.11 Update `status.yaml` and `capability-matrix.md`; record what is thinner than the tasks claim
- [ ] 7.12 `openspec validate --specs --strict` passes; archive this change
- [ ] 7.13 Open the M4 change — hot reload across the ABI with live Swift objects is its named spike
