# Tasks: VFX system

Specification-stage change. Sections 1 and 2 are the work of this change and are complete; the
change was archived on that basis.

Sections 3 to 7 record the implementation the decision implies and are **deliberately deferred to
implementation changes** — none of it can be done before there is a VFX subsystem to do it in.
They are listed so the scope is not lost. The compiler (section 3) and the global scheduler
(section 4.2) are where the risk concentrates; see `design.md`.

## 1. Specification

- [x] 1.1 Record rationale, rejected alternatives, and risks in `design.md`
- [x] 1.2 New `vfx-system` capability: asset model, GPU-first simulation, compiler and IR,
      derived attribute layout, unified world and scheduler, data interfaces, GPU scene
      integration, GPU events, bounded readback, determinism firewall, budget scalability,
      decoupled simulation rate, async compute, renderers, sorting, collision, authoring,
      cooking, gameplay API, deferred fluids, diagnostics, and validation
- [x] 1.3 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 `rendering-geometry-and-resources` — remove the superseded particle systems requirement
- [x] 2.2 `rendering-architecture` — name the GPU scene as the shared instance representation with
      multiple producers, resolving the previously implicit dependency
- [x] 2.3 `shader-system` — require engine-generated shader source (VFX kernels, material
      shaders) to be emitted as Slang through the existing pipeline, resolving the proposed
      DXC/HLSL path against the locked Slang decision
- [x] 2.4 `thirdparty-dependencies` — record the VFX runtime as engine-built, and state that
      implementing a published algorithm is not a dependency
- [x] 2.5 `build-system-and-platforms` — add `CY_VFX`
- [x] 2.6 `physics` — reviewed; no change needed. VFX consumes the scene SDF and physics queries
      through data interfaces, and the determinism firewall keeps VFX out of physics state.
- [x] 2.7 `networking-and-replication` — reviewed; no change needed. The determinism firewall in
      `vfx-system` is the requirement that protects reconciliation.
- [x] 2.8 Terminology consistency after removing the old particle requirement:
      `rendering-architecture` (object families and the instance model now say "VFX effect"),
      `rendering-2d` (the screen-space SDF is exposed as a VFX data interface),
      `editor-architecture` (VFX graph editor, sharing graph infrastructure with materials), and
      the `rendering-geometry-and-resources` purpose line.

## 3. Compiler (deferred to implementation)

- [ ] 3.1 VFX IR: typed, SSA form, stage and attribute model
- [ ] 3.2 Graph front end and node/module library format
- [ ] 3.3 Attribute liveness analysis and layout derivation, with precision selection
- [ ] 3.4 Dead-code elimination and constant folding
- [ ] 3.5 Kernel fusion
- [ ] 3.6 Slang emission and integration with the shader pipeline
- [ ] 3.7 Error reporting mapped back to graph nodes and pins

## 4. Runtime (deferred to implementation)

- [ ] 4.1 Shared particle pool and SoA attribute storage with per-class reservations
- [ ] 4.2 Global scheduler with dispatch merging and indirect dispatch
- [ ] 4.3 Data interface contract and the initial interface set
- [ ] 4.4 GPU event buffers, chain depth and per-channel budgets
- [ ] 4.5 Bounded readback path
- [ ] 4.6 GPU scene publication for mesh particles
- [ ] 4.7 Budget controller and importance classes
- [ ] 4.8 Decoupled simulation rate with interpolation
- [ ] 4.9 Async compute scheduling
- [ ] 4.10 CPU simulation path, both capability fallback and CPU-visible effects

## 5. Rendering (deferred to implementation)

- [ ] 5.1 Sprite, mesh, ribbon, beam, trail renderers
- [ ] 5.2 Decal, light, volume renderers
- [ ] 5.3 GPU sorting and order-independent approximation
- [ ] 5.4 Motion vector output and TAA correctness
- [ ] 5.5 Collision: analytic, SDF, depth buffer, physics query

## 6. Tooling (deferred to implementation)

- [ ] 6.1 VFX graph editor with live preview and playback controls
- [ ] 6.2 Cost surfacing: attribute layout, per-particle size, estimated cost
- [ ] 6.3 Particle inspector and per-node value inspection
- [ ] 6.4 Cooking step, permutation enumeration, platform feature validation
- [ ] 6.5 Swift gameplay API and component surface

## 7. Validation (deferred to implementation)

- [ ] 7.1 Compiler tests: expected IR for known graphs, per optimisation pass
- [ ] 7.2 Statistical simulation tests with fixed seed and rate
- [ ] 7.3 Golden-image tests across every backend, pinned quality
- [ ] 7.4 Budget controller convergence under synthetic overload
- [ ] 7.5 Event chain bound tests
- [ ] 7.6 Benchmark: simulation and render cost at 100k / 1M / 10M particles as a regression guard
