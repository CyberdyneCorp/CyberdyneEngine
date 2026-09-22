# Proposal

## Why

Cyberdyne can already cook models and textures and can compile a material graph, but the desktop
editor stops before the ordinary artist workflow is complete. Import, extracted sub-assets,
material properties, preview reload, terrain authoring, and VFX lowering must become one visible,
non-blocking workflow without moving importer, renderer, Metal, material compiler, or VFX internals
into the editor process.

## What Changes

- Add Content Browser import and operating-system file-drop ingestion with observable asynchronous
  progress, cooperative cancellation, retained failures, and command/agent parity.
- Publish imported sub-assets as browsable stable assets and instantiate a model's complete prefab
  hierarchy with mesh and material bindings instead of reducing every import to one mesh entity.
- Extend the backend material catalogue with typed property descriptors and render generic controls,
  including project texture selection, from those descriptors.
- Connect successful material compilation to an isolated preview world, explicit artefact reload,
  runtime parameter updates, and acknowledged viewport presentation.
- Add heightfield import plus a non-destructive terrain sculpting and layer-painting editor built on
  the shared painting surface and ordinary transactions.
- Add the VFX catalogue, schema, validation and GPU lowering as the next graph-domain consumer of
  editor backend services, followed by a shared-canvas VFX editor and live preview.

## Capabilities

### New Capabilities

- None.

### Modified Capabilities

- `asset-import-pipeline`: Visible asynchronous import, file drop, sub-asset publication, and full
  prefab instantiation.
- `editor-architecture`: Generic material property controls, preview/reload integration, and usable
  terrain and VFX specialised editors.
- `terrain`: Heightfield ingestion and non-destructive sculpt/paint authoring through stable assets.
- `vfx-system`: Versioned backend catalogue and GPU-lowering operations consumable by the editor.

## Impact

- Editor shell, content-browser view models, background-operation service, document transactions,
  asset catalogue, thumbnails, specialised graph and painting surfaces.
- Import CLI JSON contract, sub-asset database/sidecars, prefab instantiation, and asset references.
- Editor-backend service schemas and both C ABI/live transports for material preview and VFX.
- Terrain asset cooking/runtime integration and VFX compiler/lowering tests on the existing Metal
  runtime. No editor dependency on renderer/compiler/Metal/VFX implementation libraries is added.
