# Tasks

## 1. Engine

- [x] 1.1 `ModifierKind::Brush` with raise, lower, smooth (Jacobi), flatten, paint and hole; `brush_falloff`, `add_brush`, `brush_weight`, `paint_texel`; derivation key and halo cover it.
- [x] 1.2 `evaluate_region`: flatten, stitch, `mesh_tile` and `build_collision` per tile, joined render mesh; `region_height_at` for flatten targets.
- [x] 1.3 `terrain.evaluate` in `cy::editor-backend` (`TerrainPreview`), with navigation marked stale for every added, removed or changed modifier.
- [x] 1.4 The hosted runtime draws the evaluated region at `TerrainAuthoring` nodes.
- [x] 1.5 Tests: `integration.terrain_authoring` (footprint per brush, byte-identical removal, hole in rendering and collision, tile-boundary smooth), `integration.editor_backend_terrain` (the service over the C ABI, stale regions, refusals, the committed panel fixture), `smoke.editor_authored_frame_vulkan` (the terrain in the viewport).

## 2. Editor

- [x] 2.1 `hole` tool, `terrain.brush.apply` and `terrain.status`; `CommandContext::terrain_status`.
- [x] 2.2 `terrain_engine::TerrainEngine`: send the stack after every change, one request in flight, decode the reply.
- [x] 2.3 The panel draws the engine's surface, holes and stale regions, and says why nothing is shown.
- [x] 2.4 Fix: a sculpt or hole stroke names no layer; regression test.
- [x] 2.5 Tests: services unit cases, MCP cases driving brushes, status and undo over the wire, panel cases.

## 3. Records

- [x] 3.1 READMEs: `editor/README.md` (Terrain tools), `src/terrain/README.md`, `src/editor_backend/README.md`.
- [x] 3.2 Snapshots `docs/design/images/editor-terrain-tools.png` and `editor-terrain-paint.png` from the engine's committed reply.
- [x] 3.3 Mutation proofs in `evidence/falsification.txt` (`evidence/mutate.py`).
- [x] 3.4 `tools/roadmap/requirements-coverage.toml`: the terrain row's modifier stack and holes; the three added requirements recorded for mapping on archive.
- [ ] 3.5 On archive, map the added requirements as the coverage file's comment says.
