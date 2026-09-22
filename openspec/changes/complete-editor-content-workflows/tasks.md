## 1. Visible asynchronous import

- [x] 1.1 Add an Import… interaction and external-file drop handling to the Content Browser, copy accepted sources into a project-owned folder, and route both through the registered import command.
- [x] 1.2 Execute imports through observable background operations with stable request IDs, per-item progress, cooperative cancellation, retained structured failures, and a responsive editor.
- [x] 1.3 Verify FBX, OBJ and texture import from button, drop, command and agent paths, including unsupported formats, missing companions, cancellation and cache hits.

## 2. Sub-assets and prefab instantiation

- [x] 2.1 Extend the import result/CLI schema with stable sub-asset ID, kind, name, source and dependency/binding metadata; preserve deterministic ordering and compatibility.
- [x] 2.2 Index extracted meshes, materials and textures as browsable logical assets with typed thumbnails and stable references.
- [x] 2.3 Instantiate imported prefab hierarchy, transforms, mesh bindings and material slots atomically; verify undo, save/reload and source reimport identity stability.

## 3. Typed material authoring

- [x] 3.1 Complete versioned material catalogue property descriptors with stable IDs, types, defaults, constraints, enum choices, asset-kind constraints, stage/domain metadata and capabilities.
- [x] 3.2 Decode descriptors in the editor and render generic property controls without node-name switches, including project texture selection and structured refusal of invalid values.
- [x] 3.3 Persist property edits by stable node/property identity and verify validation/compilation receives texture dependencies and survives disconnect/reconnect.

## 4. Material preview and reload

- [x] 4.1 Complete generational preview-world create/destroy, typed parameter update and explicit reload acknowledgement operations over C ABI and live transport.
- [x] 4.2 Drive compile success through preview creation/reuse and reload acknowledgement before marking the viewport current; ignore stale results and retain the previous artefact on failure.
- [x] 4.3 Verify compile, cancellation, disconnect, rejected reload, live parameter update and repeated preview lifetime on the Mac Metal runtime.

## 5. Terrain import and authoring

- [x] 5.1 Add versioned heightfield import with declared units, range, resolution, tiling and structured diagnostics for unsupported or ambiguous inputs.
- [ ] 5.2 Implement the shared painting surface and a terrain specialised editor with non-destructive sculpt modifiers, material-layer painting, brush controls, transactions and undo/redo.
- [ ] 5.3 Evaluate changed tiles asynchronously, update collision/render data through stable terrain assets, and verify save/reload, cancellation and local recomputation.

## 6. VFX catalogue and GPU lowering

- [ ] 6.1 Add versioned VFX graph/catalogue schemas with stable node/pin/property identities, typed defaults, constraints and target capability queries through editor backend services.
- [ ] 6.2 Implement VFX validation and GPU lowering to engine IR/Slang artefacts, returning dependencies, attribute layout, kernels, costs and structured diagnostics.
- [ ] 6.3 Add the shared-canvas VFX editor with typed controls, compilation, live preview/reload and capability refusals; verify the Metal path without linking VFX/compiler/renderer code into the editor.

## 7. Validation and documentation

- [x] 7.1 Run strict OpenSpec validation, ABI/identity gates, focused C++ and Rust suites, and cognitive-complexity checks for changed production code.
- [ ] 7.2 Update editor/import/material/terrain/VFX documentation and capture screenshots of each completed visible workflow.
