# Proposal: VFX and vertex material graph authoring

## Why

The engine compiles VFX stage graphs, but the editor cannot open a VFX graph. The material editor can author surface graphs, but vertex animation still requires handwritten shader source. Issue #15 asks for complete authoring paths through the existing compiler and shared node canvas.

## What Changes

- Add system, emitter, stage, and reusable module authoring to the shared editor graph canvas, with a catalogue obtained from the VFX compiler registry through editor backend services.
- Add typed VFX parameters, data interfaces, renderer settings, CPU/GPU target selection, compiler diagnostics, engine-backed preview, debugging, transactions, MCP commands, save/reopen, and cook integration.
- Add a vertex stage to material graphs with world-position offset, custom interpolants, and displacement, including geometry-source variant and unsupported-path reporting.
- Make displaced geometry, shadows, and motion vectors agree, and verify the result with image and motion tests.

## Capabilities

### Modified Capabilities

- `vfx-system`: Editor-owned authoring and engine compilation/preview of VFX assets.
- `material-compiler`: Vertex graph compilation and declared geometry-source support.
- `editor-architecture`: Shared-canvas editors, transactions, Inspector controls, and diagnostics.

## Impact

The change touches the VFX compiler and service boundary, material compiler and renderer, editor canvas/panels/services/MCP, asset cooking, samples, tests, and documentation. It extends existing backend-service contracts and does not add a VFX compiler or simulator inside the editor.
