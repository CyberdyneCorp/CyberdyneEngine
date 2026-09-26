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
- [ ] 2.1b Author separately saved modules, typed parameters, interface bindings, renderer settings, and CPU/GPU targets through transactions and undo/redo.
- [x] 2.2 Validate and compile through `cy::vfx-compiler`; expose node-located `CompileReport` diagnostics, attribute layout, and generated source.
- [x] 2.3 Save, reopen, cook, and render a two-emitter CPU/GPU sample; add image comparison.
- [x] 2.4 Make parameter edits update a running effect without compilation and graph edits recompile; test the distinction.
- [x] 2.5 Add engine-backed preview controls and bounded particle, budget, event, and attribute inspection.
- [ ] 2.6 Expose equivalent VFX editing through MCP and verify undo/redo and MCP parity.

## 3. Vertex-stage material graphs

- [ ] 3.1 Add stage-aware material catalogue nodes and outputs for offset, custom interpolants, and displacement, including time, geometry attributes, noise, wind, and math.
- [ ] 3.2 Compile vertex expressions and report geometry-source variants; refuse unsupported paths in editor and cook with tests.
- [ ] 3.3 Apply offset consistently to visible geometry, shadows, and motion vectors; compare with CPU-displaced reference geometry.
- [ ] 3.4 Preview vertex graphs on the material mesh and scene, and add transaction/MCP parity tests.

## 4. Acceptance evidence

- [ ] 4.1 Add executable ledger criteria for each issue #15 acceptance criterion and record a red mutation for each.
- [ ] 4.2 Update editor and authoring documentation and sample instructions; validate OpenSpec strictly, run relevant C++/Rust/render tests, and measure changed-function cognitive complexity.
