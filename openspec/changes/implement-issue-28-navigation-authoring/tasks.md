# Tasks

## 1. Engine bake API (`src/navigation`)

- [x] 1.1 Add `NavBakeSettings` and `bake_tiles`. The bake tiles the source region, reports per-tile progress through an observer, reports empty tiles without failing, and supports cooperative cancellation.
- [x] 1.2 Add `tile_digest` and `mesh_tile_digest`. Test that a bake equals a direct `build_tile` for the same inputs, compared tile by tile, on both the Engine and the Recast back ends.
- [x] 1.3 Add `rebake_tiles` for a dirty region and the obstacle affected-tile report. Test that only the overlapping tiles change digest or salt.
- [x] 1.4 Assign NavArea volumes to triangles by centroid at build time, and build the world area-cost table. Test the area of a rebuilt tile and the path cost.
- [x] 1.5 Add `source_fingerprint` and the bake identity. Test that the fingerprint changes when geometry, a volume or a setting changes, and does not change when only an obstacle or a link does.
- [x] 1.6 Add the `.cynavmesh` codec. Test a round trip, and test that a corrupt tile, an unknown version and a truncated blob are refused.
- [x] 1.7 Report the effective area in `draw_navigation_mesh` polygons. Update `debug.h` and add a regression test for an obstacle-marked polygon.

## 2. Engine editor service and cook (`src/editor_backend`, `tools/build`)

- [x] 2.1 Add `NavigationSourceRuntime` and `NavigationService`. Implement the `navigation.*` operations, PROGRESS events per tile, stable failure codes and `capabilities.get` gating.
- [x] 2.2 Add `CompositeEditorService`. It routes by longest prefix, keeps one child session per backend and merges capabilities. Test it with MaterialService and NavigationService together.
- [x] 2.3 Add service tests: a bake equals `build_tile` tile by tile, an obstacle added through the service blocks a path and removing it restores the path, a stale status after a source change, cancellation, `navigation.busy`, and a missing seam.
- [x] 2.4 Add the `navmesh` cook producer and `kNavmeshProducerVersion`. Test identity validation, corrupt refusal, and a node key that changes on rebake.

## 3. Runtime host (`samples/05b-editor-window/runtime`)

- [x] 3.1 Implement the host seam over the authored world. It gathers MeshRenderer triangles, the Nav* components by name, sidecar store and load, and the pick ray.
- [x] 3.2 Bind the composite backend. Drain service events every frame, so PROGRESS and terminal events reach the editor.
- [x] 3.3 Draw `NavDebugSink` overlays onto the frame canvas for each enabled navigation world. Add an image test on a known map that checks the covered pixels against the projected walkable polygons.
- [x] 3.4 Run incremental updates on synced Nav* and geometry changes, and redraw the overlay live. Add a test that only the affected tiles are rebuilt and that undo restores them.
- [x] 3.5 Resolve `navigation.point.pick` against the frame's pick view. Test a hit and a miss.
- [x] 3.6 Add a runtime test that bakes a `.cyworld` test map through the service and compares it with `build_tile` tile by tile. The same test detects a stale bake after a mesh moves.

## 4. Editor services, commands and MCP (`cy-editor-services`)

- [x] 4.1 Declare the NavigationWorld, NavMeshSurface, NavObstacle, NavArea and NavLink schema, the `NavmeshSettings` read model, and the settings, overlay and component commands, with one transaction per gesture.
- [x] 4.2 Add the `NavmeshService` client, decoding the bake progress, report and failure payloads. `navigation.bake` records one transaction on Completed. Add `navigation.bake.status` and the query commands.
- [x] 4.3 Add registration and command-count updates, backend unit tests, and command unit tests covering undo and redo.
- [x] 4.4 Add MCP wire tests with a pipe-based fake runtime. The tools must appear in `tools/list`. Settings, component and bake edits must undo and redo over MCP, matching the desktop history.

## 5. Navigation panel and viewport modes (`cy-editor-interface`, `cy-editor-shell`)

- [ ] 5.1 Open `Domain::NavigationBaking` as a form editor. Add the panel title, the built-in panel kind and the docking tab.
- [ ] 5.2 Build the `panels/navigation_baking.rs` panel: world picker, settings, Bake progress and diagnostics, stale badge, overlay toggles, component add buttons, and test path and flow-field controls. The panel only pushes intents.
- [ ] 5.3 Add the armed viewport pick mode for test-path endpoints and the two-point NavLink placement.
- [ ] 5.4 Add accessibility coverage, show the Nav* types in the Inspector, and update the `PLANNED_VIEWS` navigation note.

## 6. Acceptance evidence and documentation

- [ ] 6.1 Add `tools/issue28_acceptance.py` with one probe per acceptance criterion, a native assertion-count floor, and `--no-skip`. Add its unit tests and a quality recipe.
- [ ] 6.2 Record a red mutation for each criterion in `verification.md`, with each mutation restored afterwards.
- [ ] 6.3 Update the documentation: the READMEs for `src/navigation`, `src/editor_backend` and `tools/build`, the editor feature map and the runtime sample instructions. Run `openspec validate --strict`, and measure the cognitive complexity of changed functions.
