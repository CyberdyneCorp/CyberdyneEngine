# Proposal: Finish the terrain authoring tools

## Why

Issue #29 lists the terrain editor as unfinished. The panel on the specialised-editor scaffold
(#29 Wave 0) records strokes as modifiers in the document, but nothing evaluates them. There are no
holes. The viewport shows nothing of the terrain. Nothing tells navigation that an edit made it
stale. The panel also sent the selected material layer with every stroke, so once a terrain had a
layer, every sculpt stroke was refused.

## What Changes

- `cy::terrain` gains `ModifierKind::Brush`: raise, lower, smooth, flatten, paint and hole under a
  stroke of dabs with radius, strength, falloff and pressure. A brush writes nothing outside the
  union of its dabs' discs. A smooth brush runs Jacobi passes, which its declared halo covers, and
  `paint_texel` blends a layer into the bounded texel.
- `cy::terrain` gains `evaluate_region`. It evaluates an author's square region of tiles from the
  stack, stitches heights, texels and holes, and meshes and collides every tile with the engine's
  own `mesh_tile` and `build_collision`.
- `cy::editor-backend` gains `terrain.evaluate`. The editor sends the whole ordered stack after
  every change. The engine answers with the heights, texels and holes it evaluated, what rendering
  and collision left open, and the regions whose navigation is stale. The service marks each
  modifier that appears, disappears or changes on a `terrain::TerrainNavigation`. The regions stay
  marked until navigation is rebaked, which is #28's work.
- The hosted runtime draws the engine's meshed region at every `TerrainAuthoring` node.
- The editor adds the `hole` tool, `terrain.brush.apply` (the stroke as numbers an MCP client can
  write) and `terrain.status` (what the engine last evaluated). It sends the stack to the engine
  after every change, including undo and redo. The terrain panel draws the engine's surface, holes
  and stale regions in its brush field.
- Fix: the panel names a layer only on a paint stroke.

## Capabilities

### Modified Capabilities

- `editor-architecture`: the terrain editor's brushes are evaluated by the engine, undoable, and
  MCP peers of the panel.
- `terrain`: brush modifiers and an author's region evaluation, with navigation marked stale for
  every edited region.

## Impact

Engine: `src/terrain` (brush modifier, region), `src/editor_backend` (terrain service), the hosted
runtime in `samples/05b-editor-window/runtime`. Editor: `cy-editor-services` (terrain commands and
engine view), `cy-editor-commands` (`CommandContext::terrain_status`, defaulted), `cy-editor-shell`
(terrain panel), with tests in `cy-editor-mcp`. The live protocol is unchanged: `terrain.evaluate`
travels as an ordinary service request. The `capabilities.get` operation list grows by one entry.
