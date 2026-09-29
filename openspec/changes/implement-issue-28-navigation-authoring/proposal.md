# Proposal: Navigation mesh baking and authoring in the editor

## Why

The engine's navigation stack (`src/navigation`) builds tiled navmeshes, applies dynamic obstacles, areas and off-mesh links, answers A* plus funnel queries and flow fields, and draws renderer-neutral debug overlays. The editor exposes none of it. `Domain::NavigationBaking` is registered but no panel opens it, so a level designer cannot bake, see or tune a navmesh. Issue #28 asks for that editor side. For the M12 RTS it is the first tool a level designer needs.

## What Changes

- **Engine bake API.** `cy::navigation` gains a tiled bake over a source region (`bake_tiles`), an incremental rebake of the tiles that overlap a dirty region (`rebake_tiles`), a deterministic tile digest and a versioned tile codec (`.cynavmesh`), and a source fingerprint that covers the geometry, the surface and area volumes, and the settings.
- **Engine editor service.** `src/editor_backend` gains a `NavigationService` behind the backend-service ABI (`navigation.*` operations). It reports per-tile bake progress and diagnostics, answers path and flow-field queries, resolves viewport points on the navmesh and reports whether the saved bake is stale. It gets its geometry through a `NavigationSourceRuntime` host seam. A `CompositeEditorService` routes each request by operation prefix, so the runtime can bind one backend that serves both the material/VFX service and the navigation service.
- **Cook.** A `navmesh` cook producer validates the saved `.cynavmesh` bake and adds its digest and the producer version to the world's cook identity.
- **Runtime host.** The editor-window runtime implements the host seam from the authored world (MeshRenderer geometry plus the Nav* components). It draws the navmesh overlay per navigation world with a CPU `NavDebugSink` onto the frame canvas, and it re-bakes only the tiles affected by an edit to an obstacle, an area or a surface.
- **Editor.** A Navigation panel (`editor-navigation-baking`) and `navigation.*` commands in the shared command registry cover world selection, agent profile and build settings, bake with progress and diagnostics, overlay toggles, component authoring (NavMeshSurface, NavObstacle, NavArea, NavLink), and a two-point test path with an optional flow field. Every mutation is a document transaction, so it can be undone. Each command reaches MCP through the registry projection.
- **Evidence.** An executable acceptance ledger (`tools/issue28_acceptance.py`) with one probe per acceptance criterion. `verification.md` records a red mutation for each.

## Capabilities

### Modified Capabilities

- `navigation`: bake API, tile identity and codec, incremental rebake, source fingerprint, cooked navmesh asset.
- `editor-architecture`: navigation specialised editor, commands, undo/redo and MCP parity, persistence and stale-bake reporting.
- `editor-viewport-and-gizmos`: engine-drawn navmesh overlay per navigation world, engine-side navmesh point picking, and test-path and link-placement modes.

## Impact

- **Code:** `src/navigation`, `src/editor_backend`, `tools/build` (the producer), `samples/05b-editor-window/runtime`, and the editor crates `cy-editor-services`, `cy-editor-interface` and `cy-editor-shell`. The editor crates change by appending files and registrations only.
- **Contracts:** it adds backend-service operations under schema 1 and does not change the C ABI.
- **Ownership:** the editor never bakes, hashes geometry, unprojects or raycasts. The engine does all of these, and the editor requests them and displays the results.
