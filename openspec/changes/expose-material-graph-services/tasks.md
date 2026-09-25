## 1. Freeze the editor-service contract

- [ ] 1.1 Define the versioned request, cancellation, event, diagnostic, artefact, dependency, capability, catalogue, preview, parameter-update, and reload-acknowledgement schemas in the engine-owned contract; verify deterministic round trips and rejection of unsupported schema versions.
- [ ] 1.2 Add manifest-assigned `NodeTypeId` and `PinId` values to CyberGraph descriptors without replacing human-readable names; verify rename stability, duplicate rejection, and tombstone/non-reuse rules in the identity gate.

## 2. Carry the contract across both supported boundaries

- [x] 2.1 Append ABI 1.2 service-session open, close, submit, cancel, and poll entries using only opaque handles and POD values; regenerate the ABI baseline and Rust SDK, then run the ABI layout and append-only gates.
- [x] 2.2 Append live-protocol service request, cancellation, and event messages without changing existing tags; verify Rust encode/decode round trips and byte-for-byte C++/Rust fixtures.
- [ ] 2.3 Route both transports through the same backend service dispatcher and prove duplicate request IDs, cancellation races, and terminal-event publication have identical semantics.

## 3. Expose material authoring services

- [ ] 3.1 Register a deterministic, versioned material-node catalogue containing stable node/pin identities, types, typed defaults, constraints, stage/domain metadata, and capability requirements; verify it covers every material node accepted by lowering.
- [ ] 3.2 Implement material graph validation with stable diagnostic codes and node/pin source locations; verify invalid links, missing output, unsupported nodes, and target capability failures produce structured diagnostics.
- [ ] 3.3 Implement asynchronous material compilation through the service dispatcher and return content/derivation identities, dependency identities, stages, cost metadata, and diagnostics; verify equivalent inputs produce identical identities and cancelled work publishes no successful artefact.

## 4. Implement preview and hot-reload lifecycle

- [ ] 4.1 Implement isolated, generational preview-world create/destroy operations with idempotent destruction and stale-handle rejection; verify repeated create/destroy cycles do not leak worlds or cross-contaminate state.
- [ ] 4.2 Implement typed runtime parameter updates and explicit reload acknowledgements carrying requested and applied artefact identities; verify type mismatches, stale previews, unsupported parameters, and rejected reloads remain structured failures.

## 5. Consume the backend contract in the editor

- [x] 5.1 Replace the Rust editor's copied material catalogue with catalogue data returned by the backend, retaining only presentation metadata in the editor; verify a backend catalogue addition appears without an editor code change.
- [ ] 5.2 Surface structured validation/compilation diagnostics and capability refusals in the material document while preserving unsaved graph edits when a runtime disconnects; verify the authoring state survives service loss.
- [x] 5.3 Expose `MeshRenderer.material` in the editor document schema and route material asset drops through a kind-checked, undoable assignment command; verify mesh preservation, undo, save/reload, and wrong-kind refusal.
- [x] 5.4 Render a backend-populated Material Graph panel on the shared `GraphCanvas`, with palette insertion, stable node/pin presentation, and disconnect-safe authored state; verify a backend-only node is visible and nodes survive service loss.
- [x] 5.5 Add stable-ID pin interaction and navigable shared-canvas preflight diagnostics to the Material Graph panel; verify compatible links persist and incompatible links remain non-mutating, visibly refused edits.
- [x] 5.6 Submit the visible material canvas to asynchronous validation and compilation operations, correlate terminal events by request identity, and show validation or compiled artefact state without linking compiler code into the editor; verify authored state survives failures and disconnects.
- [x] 5.7 Carry ordered, versioned material diagnostics with stable codes and node/pin locations through the backend result, and make those locations navigable in the Material Graph; verify legacy failures remain readable and an invalid graph selects its responsible node without mutating authored state.
- [x] 5.8 Add a Plane/Cube material-graph sample, reopen its material nodes from the selected mesh, and display a constant-color Diffuse graph in the authored scene renderer. Keep unsupported graphs explicit until the authored renderer consumes compiled shader variants.

## 6. Validate and document the vertical slice

- [x] 6.1 Add an end-to-end Mac/Metal test that opens a service session, obtains the material catalogue, validates and compiles a material, creates a preview, updates a parameter, acknowledges reload, and destroys the preview.
- [ ] 6.2 Run `openspec validate expose-material-graph-services --strict`, the ABI/identity gates, focused C++ and Rust suites, and cognitive-complexity measurement for changed production code; record any genuinely irreducible exception.
- [x] 6.3 Update boundary/module documentation and mark M11.e material command-front-end tasks 5a.1–5a.3 as superseded by this change while keeping VFX GPU lowering explicitly outside this first vertical slice.
