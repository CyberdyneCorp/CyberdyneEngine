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

- [x] 5.1 Open `Domain::NavigationBaking` as a form editor. Add the panel title, the built-in panel kind and the docking tab.
- [x] 5.2 Build the `panels/navigation_baking.rs` panel: world picker, settings, Bake progress and diagnostics, stale badge, overlay toggles, component add buttons, and test path and flow-field controls. The panel only pushes intents.
- [x] 5.3 Add the armed viewport pick mode for test-path endpoints and the two-point NavLink placement.
- [x] 5.4 Add accessibility coverage, show the Nav* types in the Inspector, and update the `PLANNED_VIEWS` navigation note.

## 6. Acceptance evidence and documentation

- [x] 6.1 Add `tools/issue28_acceptance.py` with one probe per acceptance criterion, a native assertion-count floor, and `--no-skip`. Add its unit tests and a quality recipe.
- [x] 6.2 Record a red mutation for each criterion in `verification.md`, with each mutation restored afterwards.
- [x] 6.3 Update the documentation: the READMEs for `src/navigation`, `src/editor_backend` and `tools/build`, the editor feature map and the runtime sample instructions. Run `openspec validate --strict`, and measure the cognitive complexity of changed functions.

## 7. Review fixes

- [x] 7.1 Restore a bake with the area costs of the current sources (`restore_bake`), so a reopened world or an undone bake keeps its NavArea multipliers. Regression: `editor_backend: a restored bake keeps its area costs in a new session and on undo`.
- [x] 7.2 Add `rebake_surface_tiles`: an incremental rebuild clipped to the surface region, which removes the tiles a shrunk or moved surface no longer covers. Regressions: the two `incremental surface rebake` cases in `test_bake.cpp` and `editor runtime: shrinking the surface leaves the mesh equal to a fresh bake of the shrunk map`.
- [x] 7.3 Rebake the whole surface region when the dirty box cannot vouch for the result (a stale restore, or a change in the geometry's height range), so the recorded fingerprint never names a hybrid mesh. Regressions: `editor_backend: an update after restoring a stale bake rebuilds every changed tile, not only the dirty box` and `editor_backend: an edit that changes the geometry's height range rebuilds every tile`.
- [x] 7.4 Add `navigation.clear` and send it when a world's recorded bake goes from non-zero to zero, so undoing the first bake leaves the engine unbaked. Regressions: `editor_backend: navigation clear drops the mesh and refuses later queries` and `editor runtime: undoing the first bake drops the engine's mesh and refuses path queries`.
- [x] 7.5 Voxelise Recast tiles with a `walkableRadius + 3` border so tiles connect across seams (`borderSize` was 0 on main, confirmed on `origin/main`). Regression: `a two-tile Recast bake connects its tiles across the seam`.
- [x] 7.6 Build the probed test binaries before the ledger runs them (`--no-build` opts out), add `--native-only`, and run it in CI's test job (`just quality-issue28-native`, linux-x86_64).
- [x] 7.7 Criterion 2 on the known test map: extract `draw_editor_navigation` from `main.cpp`'s `draw_frame_overlays` and drive it over the baked `nav_test_map.cyworld`, checking covered pixels and the ground, area and carved-footprint colours.
- [x] 7.8 Criterion 3 end to end: a NavObstacle added by document text blocks the runtime's path and removing it restores it (`test_nav_runtime.cpp`).
- [x] 7.9 Guard follow-on code after `CY_REQUIRE` in the navigation tests (exception-free build): `worlds[0]`, `std::map::at`, and the rim pixel's `x - 1`.
- [x] 7.10 Report a missing sidecar from `navigation.status` as `baked = 0`, `sidecar missing = 1` and the current fingerprint instead of failing; decode it in `NavStatusReport`, show it in the panel, and document that sidecars are committed.
- [x] 7.11 Keep a runtime request the service refuses at submit and retry it next frame. Regression: `editor runtime: a runtime request the service refuses at submit is kept and retried`.
- [x] 7.12 Clip overlay polygons at the near plane instead of dropping them. Regression: `nav overlay clips a polygon at the near plane with the camera inside the tile`.
- [x] 7.13 Clear the awaited pick when the shell's `navigation.point.pick` invoke is refused, and rescale a click only with the frame it was made on. Regressions: `a_refused_pick_is_not_settled_by_a_later_unrelated_answer` and `a_click_is_rescaled_only_with_the_frame_it_was_made_on`.
- [x] 7.14 State in the guide that cost painting is by volumes only.
- [x] 7.15 Run the ledger's CI step even when `just test-all` failed (`!cancelled()`); PR #64's first run skipped it behind main-owned failures. Regression: `test_ci_runs_the_native_probes_even_after_a_failed_suite`.
- [x] 7.16 Keep the overlay image tests inside the Debug case budget: the `profiles` job's Debug run failed `the frame overlay over the baked test map ...` at 4159 ms of CPU against 4000 ms. The per-pixel scans now test a polygon only when its pixel bounds hold the centre; assertion counts are unchanged. Regression: the harness budget itself, checked with `CY_TEST_BUDGET_SCALE=0.5` on a Debug build.

## 8. Deferred

- [ ] 8.1 Declare a `navmesh` node automatically for every world with a `NavigationWorld`, so the world's cook identity includes its navmesh without a hand-written node. Needs the world cook to enumerate a world's components; outside this change.
- [ ] 8.2 Load the cooked `.cynavmesh` into a game world at runtime. Needs the game runtime's navigation bootstrap; outside this change.
- [ ] 8.3 Observe `main.cpp`'s three-line `draw_navigation` wiring (the frame's `host.view` and `eye_of(host.camera)`) on a device. Everything below that call is covered by `draw_editor_navigation`'s image test; the wiring itself is the same view and eye `record_frame` hands the pick path.
- [ ] 8.4 Release: push the branch, open the PR with a descriptive body, watch CI and merge. Not done in this session (no push was requested).
