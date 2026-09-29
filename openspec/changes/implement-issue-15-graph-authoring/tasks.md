# Tasks

## 1. VFX catalogue and shared canvas

- [x] 1.1a Publish the registered VFX node names, stable identities, and typed pins through `vfx.catalogue.get`; test every entry against `register_vfx_nodes`.
- [x] 1.1b Add typed VFX node property descriptors and populate the sample interface choices from the registered data-interface catalogue.
- [x] 1.1c Generate typed sample nodes for registered interface fields and lower them through the existing VFX compiler; verify a project-defined interface.
- [x] 1.1d Expose renderer availability and reasons, and target capability queries from their owning registries.
- [x] 1.2a Fetch/install the catalogue in the editor and open `Domain::VfxGraph` on the shared canvas; test backend-only node discovery and missing-backend refusal.
- [x] 1.2b Make the VFX graph panel reachable in the desktop UI with shared-canvas node and property editing, service status, and accessibility coverage. Draft persistence remains task 2.1.

## 2. VFX documents and engine services

- [x] 2.1a Add a versioned system/emitter/stage authoring document, shared-canvas stage switching, and project draft save/reopen through undoable commands.
- [x] 2.1b Author separately saved modules, typed parameters, interface bindings, renderer settings, and CPU/GPU targets through transactions and undo/redo.
  - [x] 2.1b.1 Resolve saved `.cyvfxmodule` assets at cook time with emitter-scoped diagnostics, declared-stage node checks, and transitive content digests in the cook key (#19).
  - [x] 2.1b.2 Discover referenced modules through the build graph so a changed module re-cooks its users (#19).
  - [x] 2.1b.3 Save, reopen, undo, and redo a module and a system that uses it over MCP (#19).
- [x] 2.2 Validate and compile through `cy::vfx-compiler`; expose node-located `CompileReport` diagnostics, attribute layout, and generated source.
- [x] 2.3 Save, reopen, cook, and render a two-emitter CPU/GPU sample; add image comparison.
- [x] 2.4 Make parameter edits update a running effect without compilation and graph edits recompile; test the distinction.
- [x] 2.5 Add engine-backed preview controls and bounded particle, budget, event, and attribute inspection.
- [x] 2.6 Expose equivalent VFX editing through MCP and verify undo/redo and MCP parity.
- [x] 2.7 Author emitter-local typed parameters beside shared system parameters; preserve their identities through save/reopen, compiler, cook, preview, and runtime lookup.
- [x] 2.8 Add a serializable scene effect binding with per-instance exposed-parameter overrides; edit it in the Inspector and MCP, and verify two scene instances retain different values through save/reopen and runtime load.
- [x] 2.9 Append effect-instance parameter set/get to the C ABI and generated Swift overlay; prove a Swift module changes one live instance without recompiling or changing another.

## 3. Vertex-stage material graphs

- [x] 3.1 Add stage-aware material catalogue nodes and outputs for offset, custom interpolants, and displacement, including time, geometry attributes, noise, wind, and math.
- [x] 3.2 Compile vertex expressions and report geometry-source variants; refuse unsupported paths in editor and cook with tests.
- [ ] 3.3 Apply offset consistently to visible geometry, shadows, and motion vectors; compare with CPU-displaced reference geometry.
- [ ] 3.4 Preview vertex graphs on the material mesh and scene, and add transaction/MCP parity tests.
- [ ] 3.5 Bind the environment-field table on native Metal (the `CyFrameGlobalSet` and editor material-texture argument buffers) and restore the wind-field image comparison in `smoke.editor_material_metal`, which currently asserts the refusal.

## 4. Acceptance evidence

- [ ] 4.1 Add executable ledger criteria for each issue #15 acceptance criterion and record a red mutation for each.
- [ ] 4.2 Update editor and authoring documentation and sample instructions; validate OpenSpec strictly, run relevant C++/Rust/render tests, and measure changed-function cognitive complexity.
- [ ] 4.3 Add executable ledger probes and red mutations for scene instance and Swift VFX parameter paths found in issue #15's scope audit.
