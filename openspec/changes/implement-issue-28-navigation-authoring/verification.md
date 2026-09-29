# Verification ledger

This ledger records the executable evidence for issue #28. Every acceptance criterion has a green check and a recorded red mutation.

- `python3 tools/issue28_acceptance.py --list` lists the criteria.
- `python3 tools/issue28_acceptance.py` runs their probes and exits nonzero while any criterion is unverified.
- Native probes run with `--no-skip`. Each has an assertion-count floor, so that a skip on a null device cannot pass.
- Every mutation below was executed: applied to the source, shown red, and restored, with the restored probe shown green. None of them is committed.

## Engine bake API (tasks 1.1 to 1.7)

These are the engine-level checks under criteria 1, 3 and 5, plus the overlay's effective-area
regression. They run without an editor or a device.

- **Green:** `cmake --build build/dev --parallel 8`, then
  `ctest --test-dir build/dev -R navigation --output-on-failure`. `integration.navigation_bake` ran 13
  cases with 50203 assertions, and `unit.navigation` includes the codec and debug cases. The Recast
  case ran on this build (`recast_available()`, 4642 assertions). On a build without Recast it
  prints a SKIP message instead.
- **Red mutations.** Each mutation was applied to the source, the suite was rebuilt and run with the
  named case filter, and the source was restored. The restored suites pass.

| # | Check (case) | Mutation | Failing assertion |
|---|---|---|---|
| M1 | `a bake equals build_tile tile by tile on the engine back end` | `inputs.params.cell_size *= 1.01F` in `prepare` (bake.cpp) | test_bake.cpp `CHECK_EQ(report->tiles[index].digest, expected)`: 16 failures |
| M2 | `an empty tile inside the region is reported and the bake succeeds` | an empty tile returns an error in `bake_one` | `REQUIRE(report.has_value())` |
| M3 | `an observer cancels a bake after the tile it is told about` | the observer's `Cancel` answer is ignored | `CHECK(report->cancelled)`, `CHECK_EQ(counter.calls, 1U)` |
| M4 | `an area volume edit rebakes only its tile and the others keep digest and salt` | a rebake grows the dirty box by one tile in x and z | `REQUIRE_EQ(report->tiles.size(), usize{1})` (got 4), `CHECK_EQ(now.salt, before[index].salt)` |
| M5a | `an obstacle over a corridor blocks the path and removing it restores the cost` | `place_obstacle` reports the tiles under an empty box | `CHECK_EQ(placed->affected.size(), usize{1})` (got 0) |
| M5b | `moving an obstacle reports the tiles under its old and new footprints` | `move_obstacle` drops the old footprint's tiles | `CHECK_EQ(moved->affected.size(), usize{2})` (got 1) |
| M5c | `an obstacle over a corridor blocks the path ...` | the obstacle is applied with `kAreaGround` instead of its area | `CHECK((!blocked.found \|\| blocked.partial))` |
| M6 | `area volumes assign their area to triangles by centroid, highest node winning` | the lowest node wins | `CHECK_EQ(areas[1], AreaType{3})` (got 4) |
| M7 | `an area volume's cost changes the cost of a path across it` | `area_costs` ignores the volume's cost | `CHECK_GT(dear.cost, cheap.cost * 3.0F)` (11.75 vs 35.25) |
| M8a | `the source fingerprint follows the bake inputs and ignores obstacles and links` | the fingerprint hashes the obstacle count | `CHECK_EQ(source_fingerprint(bake, overlaid, 1), base)` |
| M8b | same case | the fingerprint leaves out the vertices | `CHECK_NE(...)` after a vertex moves (equal fingerprints) |
| M9a | `a cynavmesh with a flipped tile byte is refused as a digest mismatch` | decode skips the tile-digest comparison | `CHECK(mentions(refused.error(), "digest"))` (refused later, for the identity instead) |
| M9b | `a cynavmesh of an unknown version is refused` | decode skips the version check | `REQUIRE_FALSE(refused.has_value())` |
| M9c | `a truncated cynavmesh is refused at every cut` | decode skips the truncation check after a tile | `CHECK(refused_for(refused, "truncated"))` |
| M10 | `navigation debug reports the effective area of an obstacle-marked polygon` (regression) | `draw_polygon` reports `NavPoly::area` | test_debug.cpp `CHECK_EQ(marked.matching, 1U)` (got 0) |

The effective-area behaviour in M10 was already in `debug.cpp` on main. The regression test and
the `NavDebugSink::polygon` documentation now pin it down.

## Engine editor service and cook (tasks 2.1 to 2.4)

These are the service-boundary checks under criteria 1, 3 and 5, plus the service contract (progress,
cancellation, busy, seam gating), the composite routing and the navmesh cook producer. They run
without an editor or a device.

- **Green:** `cmake --build build/dev --parallel 8`, then
  `ctest --test-dir build/dev -R "editor_backend|build_content" --output-on-failure`.
  `integration.editor_backend_navigation` ran 16 cases (12 service, 4 composite) with 49966
  assertions; `integration.build_content` includes the 4 navmesh producer cases.
  `unit.editor_backend` and `integration.editor_backend_compile` still pass, and
  `src/editor_backend/src/material_service.cpp` is unchanged.
- **Red mutations.** Each was applied to the source, the target rebuilt and the named case run
  with `-tc=`, and the source restored; the restored suites pass.

| # | Check (case) | Mutation | Failing assertion |
|---|---|---|---|
| S1 | `editor_backend: navigation bake equals build_tile tile by tile` (criterion 1) | the settings decode reads `cell_size * 1.01` (navigation_service.cpp `read_settings`) | `CHECK_EQ(baked.tiles[index].digest, expected)` and `CHECK_EQ(nav::mesh_tile_digest(*mesh, slot), expected)`: 10 failures |
| S2 | `editor_backend: an obstacle added through the service blocks the path and removing it restores it` (criterion 3) | `sync_obstacles` drops the seam's obstacles after gathering them | `CHECK_EQ(marked, usize{1})`, `CHECK((!blocked.found \|\| blocked.partial))`, and the direct `find_path` check |
| S3 | `editor_backend: navigation status reports a stale bake after a source change` (criterion 5) | `status` reuses the saved fingerprint instead of recomputing it from the seam | `CHECK(stale.stale)`, `CHECK_NE(stale.current, baked.fingerprint)`, `CHECK(resized.stale)` |
| S4 | `editor_backend: a second navigation request while a bake is pending is busy` | `submit` accepts the second request without recording it for `navigation.busy` | `CHECK_EQ(busy.request, 21U)`, `CHECK_EQ(busy.code(), "navigation.busy")` |
| S5 | `editor_backend: navigation without a seam is unavailable and not advertised` | `capabilities.get` counts the navigation operations with no seam | `CHECK_EQ(none.size(), usize{1})` and the payload's `decoder.done()` |
| S6 | `editor_backend: navigation bake emits one PROGRESS per tile then one COMPLETED` | the tile step no longer marks its event as PROGRESS | `REQUIRE_EQ(events.size(), usize{5})` |
| C1 | `composite: capabilities.get merges every child's operations` | only the first child's answer is absorbed | `CHECK(has(names, "navigation.bake"))`, `CHECK_NE(features & kNavigationFeature, 0U)` |
| C2 | `composite: a material request succeeds while a navigation bake is pending` | `submit` refuses any request while a child has one in flight | `REQUIRE_EQ(accepted, CY_RESULT_OK)` |
| P1 | `navmesh producer: a corrupt tile or an identity mismatch fails the node` | the identity comparison is skipped | `CHECK_FALSE(other->succeeded())`, `CHECK(diagnosed(..., "navmesh-identity"))` |
| P2 | same case | a sidecar that fails to decode is written through | `CHECK_FALSE(refused->succeeded())`, `CHECK(diagnosed(..., "navmesh-sidecar"))` |

The producer's key cases (`a rebake changes the node key`, `a producer version bump changes the
node key`) check the graph's derivation key over the declared sidecar, the identity option and
`kNavmeshProducerVersion`. The producer body cannot change that key, so no producer-side mutation
applies; their guard is that the node declares the sidecar as a source.

## Runtime host (tasks 3.1 to 3.6)

These are the runtime-host checks under criteria 1, 2 and 5, plus the runtime's own contract:
incremental updates, navmesh picking, sidecar reload on a recorded-identity change, and the
per-frame drain. They run over `samples/05b-editor-window/runtime/tests/data/nav_test_map.cyworld`
through the same `CompositeEditorService` binding the runtime installs (MaterialService plus
NavigationService). They need no device: the overlay is drawn on a CPU canvas and the frame view
is recorded directly.

- **Green:** `cmake --build build/dev --parallel 8`, then
  `ctest --test-dir build/dev -R editor_window_navigation --output-on-failure`.
  `integration.editor_window_navigation` ran 9 cases (3 overlay, 6 runtime) with 17287 assertions.
  `unit.editor_window_runtime`, `integration.editor_window_overlay`, the other `editor_window_*`
  suites, `unit.editor_backend`, `integration.editor_backend_compile`,
  `integration.editor_backend_navigation`, `smoke.editor_authored_frame_metal` and
  `smoke.editor_material_metal` still pass with the runtime bound through the composite.
- **Red mutations.** Each was applied to the source, the target rebuilt and the named case run
  with `-tc=`, and the source restored; the restored suite passes.

| # | Check (case) | Mutation | Failing assertion |
|---|---|---|---|
| R1a | `editor runtime: baking the test map equals build_tile tile by tile` (criterion 1) | `append_mesh` (nav_runtime.cpp) appends mesh-local positions instead of world-space ones | `CHECK_GT(baked.tiles[index].polys, 0U)`, `REQUIRE_NE(slot, 0xFFFFFFFFU)`, `CHECK_EQ(nav::mesh_tile_digest(*mesh, slot), digest)` |
| R1b | same case | the service's settings decode reads `cell_size * 1.01` (navigation_service.cpp `read_settings`) | `CHECK_EQ(baked.tiles[index].digest, digest)`, `CHECK_EQ(baked.fingerprint, fingerprint)`, `CHECK_EQ(baked.identity, ...)`: 10 failures |
| R2 | `nav overlay covers the projected walkable polygons` (criterion 2) | `NavCanvasSink::polygon` skips every second polygon | `CHECK_LE(coverage.mismatched, coverage.expected / 100U)` |
| R3a | `editor runtime: moving a mesh marks the navigation bake stale; moving it back clears the flag` (criterion 5) | `append_mesh` ignores the node transform (as R1a) | `CHECK(runtime.stale(baked))`, `CHECK_FALSE(last_update().rebuilt.empty())`, and the restored-digest checks |
| R3b | same case | the mesh contribution's digest leaves out its world bounds, so a move is not seen as a change | `CHECK_EQ(updates_completed(), before + 1)`, `CHECK_FALSE(last_update().rebuilt.empty())` |
| R4a | `editor runtime: an area edit on the test map rebuilds only the affected tiles; reverting restores their digests` | `dirty_regions` ignores `NavArea` contributions | `REQUIRE_EQ(update.rebuilt.size(), usize{1})`, `CHECK_NE(edited_digests.at(coord), digest)` |
| R4b | same case | the queued update widens the dirty box by one tile in x and z | `REQUIRE_EQ(update.rebuilt.size(), usize{1})`, `CHECK(update.rebuilt[0] == TileCoord{0, 0, 0})` |
| R5 | `editor runtime: navigation.point.pick hits the navmesh and misses beyond it` | `pick_ray` leaves the ray at the camera-relative origin instead of the eye | `CHECK_NEAR(point.x, 8.0F, ...)`, `CHECK_NEAR(point.z, 8.0F, ...)`, `CHECK_NE(polygon, 0U)` |
| R6 | `editor runtime: an undone bake reloads its sidecar and the overlay follows the recorded identity` | a restore is queued only for a world seen for the first time, not when its recorded identity changes | `CHECK_EQ(restores_completed(), restores + 1)` |
| R7 | `nav overlay per-world toggle draws only the enabled world` | `draw_navigation_overlay` draws polygons whatever the world's flags | `CHECK_EQ(right_hidden.hit, 0U)`, `CHECK_LE(left_drawn.mismatched, ...)`, `CHECK_GT(right_drawn.drawn, left_drawn.drawn)` |
| R8 | `editor runtime: the per-frame drain forwards every bake PROGRESS and one COMPLETED` | `drain_service_events` forwards only non-PROGRESS events | `REQUIRE_EQ(forwarded.kinds.size(), usize{5})` |

## Editor services, commands and MCP (tasks 4.1 to 4.4)

These are the editor-side checks under criteria 3 and 4, plus the `NavmeshService` client contract
(request-id matching, progress, completed, failed, disconnect). The MCP probes drive the real
`McpServer` over the shared registry and answer the editor's service requests from a pipe-based
fake runtime, so they need neither an engine nor a device.

- **Green:** in `editor/`, `cargo test -p cy-editor-services -p cy-editor-commands -p cy-editor-mcp`.
  `cy-editor-services` runs 27 navigation cases (`nav_bake`, `navmesh`, `navmesh_service`) and the
  updated `every_built_in_command_satisfies_a_caller_that_cannot_see_the_interface` count (+18);
  `a_session_over_the_wire` runs `navigation_tools_are_projected_over_mcp`,
  `navigation_settings_component_and_bake_edits_undo_and_redo_over_mcp` and
  `navigation_obstacle_add_and_remove_reach_the_engine_over_mcp`. `cargo fmt --check` is clean and
  `cargo clippy -p cy-editor-services -p cy-editor-commands -p cy-editor-mcp --all-targets -- -D warnings`
  passes.
- **Red mutations.** Each was applied to the source, the named test run with `cargo test`, and the
  source restored; the restored suites pass.

| # | Check (test) | Mutation | Failing assertion |
|---|---|---|---|
| E1 | `navmesh_service::tests::completed_records_exactly_one_bake_transaction_and_undo_restores_the_identity`; MCP `navigation_settings_component_and_bake_edits_undo_and_redo_over_mcp` (criterion 4) | `take_completed` clones the completed bake instead of taking it, so every pump records it again | `assert_eq!(rig.history(), 2, "a completion is recorded once")`; over MCP `"undo restores the unbaked identity"` (a second bake entry is undone first) |
| E2 | same two tests (criterion 4) | `settle_bake` never hands the completed bake to `finish_nav_bake` | `assert_eq!(rig.history(), 2, "one bake transaction")`; over MCP `pump_until` reports `the editor did not settle the engine's answer` |
| E3 | `navmesh_service::tests::a_mismatched_request_id_is_ignored` | `accept` settles any service event while a request is pending, whatever its id | `assert_eq!(rig.editor.navmesh.pending_request(), Some(request))` |
| E4 | `navmesh_service::tests::failed_records_nothing_and_keeps_the_diagnostics` | a FAILED bake also queues a report for `finish_nav_bake` | `assert_eq!(rig.history(), 1)` |
| E5 | `navmesh_service::tests::disconnect_fails_the_pending_request` | `disconnect` fails the request but leaves it pending | `assert!(rig.editor.navmesh.pending_request().is_none())` |
| E6 | `navmesh_service::tests::progress_updates_the_progress` | a decoded PROGRESS event is dropped | `assert_eq!(progress.map(..), Some((1, 3)))` |
| E7 | MCP `navigation_obstacle_add_and_remove_reach_the_engine_over_mcp` (criterion 3, editor side) | `navigation.component.remove` records neither the component removal nor the node deletion | `"the engine received the removal before the second path query"` |
| E8 | MCP `navigation_settings_component_and_bake_edits_undo_and_redo_over_mcp` (criterion 4) | `navigation.bake` sends the default settings instead of the document's | `"the bake carries the edited settings"` |
| E9 | `navmesh::tests::a_settings_edit_is_one_entry_and_undo_redo_restore_it`; the MCP criterion-4 test | `write_fields` records one transaction per field instead of one per gesture | `"one entry per gesture"` in both |

## Navigation panel and viewport modes (tasks 5.1 to 5.4)

These are the panel-side checks under criteria 3 and 4, plus the form-editor opening, the
`SpecialisedTool` frame from #29's scaffold (header, diagnostics, MCP and undo parity), the
accessible empty state, the armed viewport pick and the Inspector exposure. They run headless
(egui with AccessKit, no device); engine answers to `navigation.point.pick` arrive as service
events on a pipe-backed runtime session.

- **Green:** in `editor/`, `cargo test -p cy-editor-interface -p cy-editor-shell -p cy-editor-viewport -p cy-editor-services`.
  `cy-editor-interface` runs `specialised::tests::navigation_baking_opens_as_a_form_editor`;
  `cy-editor-shell` runs `tests/navigation_baking_panel.rs` (10 tests), the scaffold's
  `every_scaffolded_tool_is_an_undoable_mcp_peer_of_its_panel` (which now registers `NavigationTool`) and
  `new_panels_are_accessible` with `("editor-navigation-baking", "No world is open.")`;
  `cy-editor-services` runs
  `navmesh_service::tests::every_ended_pick_is_counted_and_a_refused_one_leaves_no_stale_point`.
  `cargo fmt --check` is clean and
  `cargo clippy -p cy-editor-interface -p cy-editor-shell -p cy-editor-viewport -p cy-editor-services --all-targets -- -D warnings`
  passes.
- **Red mutations.** Each was applied to the source, the named test run with `cargo test`, and the
  source restored; the restored suites pass.

| # | Check (test) | Mutation | Failing assertion |
|---|---|---|---|
| P1 | `navigation_baking_opens_as_a_form_editor` | `can_open` drops `\|\| domain == Domain::NavigationBaking` | `.expect("navigation baking opens")` |
| P2 | `new_panels_are_accessible::every_new_panel_survives_the_theme_density_width_matrix_with_accessible_names` | the panel's empty state says "Nothing is open." | the harness finds no accessible "No world is open." for `editor-navigation-baking` |
| P3 | `bake_settings_overlays_and_add_buttons_push_their_navigation_commands` | Bake pushes `navigation.bake` without its `world` | `assert_eq!(bake.get("world"), Some(&Value::Int(1)))` (got `None`) |
| P4 | same test | `changed_settings` sends every setting, changed or not | `"the gesture sends exactly the edited setting"` |
| P5 | `an_armed_viewport_click_picks_a_navmesh_point_instead_of_selecting` | `armed_pick` ignores every target but `LinkTo` | `expected one navigation.point.pick, got []` |
| P6 | same test | `report` sends the armed click to `navigation.point.pick` and then also to `request_pick` | `"the armed click also went to selection picking"` |
| P7 | `two_picks_in_link_mode_record_one_link_add` | the first link pick records a `navigation.link.add` of its own | `"one endpoint records nothing"` |
| P8 | `picked_path_endpoints_feed_the_path_query` | `settle_pick` stops comparing `pick_answers` with the count at send time | `"the panel settled Pick start before the engine answered"` |
| P9 | `the_inspector_shows_and_undoably_edits_a_nav_obstacle` | the `navigation.*.add` commands stop selecting the node they create | `"the Inspector does not show NavObstacle"` |
| P10 | `panel_gestures_record_the_history_the_same_gestures_record_over_mcp` (criterion 4) | Apply settings sends only the first changed setting | `assert_eq!(from_panel.settings, from_agent.settings)` (`tile_size` 16 vs 8) |
| P11 | `every_ended_pick_is_counted_and_a_refused_one_leaves_no_stale_point` | a refused pick keeps the previous answer in `NavmeshService::pick` | `"a refused pick left the previous point to be read as its answer"` |
| P12 | `panels::specialised::tests::every_scaffolded_tool_is_an_undoable_mcp_peer_of_its_panel` | `NavigationTool::COMMANDS` names a command nobody registers (`navigation.point.teleport`) | `.expect("parity holds for the shipped tools")` (the refusal names the command) |
| P13 | `the_panel_draws_in_the_specialised_frame_with_its_problem_in_the_diagnostics_area` | `NavigationTool::diagnostics` drops the panel's problem | `"the problem is a readable diagnostics row"` |

## Acceptance ledger (task 6.1 and 6.2)

`tools/issue28_acceptance.py` holds one criterion per acceptance point of issue #28. Each criterion
names its probes and the red mutation recorded for it. A native probe runs one doctest case with
`--no-skip` and must report exactly one executed case and at least its assertion floor. A Cargo
probe must execute at least one test. The runner exits nonzero while any criterion is unverified.

- **Green:** `cmake --build build/dev --parallel 8`, then `python3 tools/issue28_acceptance.py`:
  `verified 6/6 selected criteria`, exit 0. Floors against the counts measured on this tree:

| Criterion | Probe | Assertions (floor) |
|---|---|---|
| 1 `bake` | `editor_backend: navigation bake equals build_tile tile by tile` | 4654 (1000) |
| 1 `bake` | `editor runtime: baking the test map equals build_tile tile by tile` | 74 (40) |
| 2 `overlay` | `nav overlay covers the projected walkable polygons` | 5255 (1000) |
| 2 `overlay` | `nav overlay per-world toggle draws only the enabled world` | 10504 (10) |
| 3 `obstacle` | `editor_backend: an obstacle added through the service blocks the path and removing it restores it` | 1195 (200) |
| 3 `obstacle` | cargo `cy-editor-mcp navigation_obstacle_add_and_remove_reach_the_engine_over_mcp` | 1 test |
| 4 `history` | cargo `cy-editor-mcp navigation_settings_component_and_bake_edits_undo_and_redo_over_mcp`, `navigation_tools_are_projected_over_mcp`; `cy-editor-services completed_records_exactly_one_bake_transaction_and_undo_restores_the_identity`, `a_settings_edit_is_one_entry_and_undo_redo_restore_it`; `cy-editor-shell panel_gestures_record_the_history_the_same_gestures_record_over_mcp` | 1 test each |
| 5 `stale` | `editor_backend: navigation status reports a stale bake after a source change` | 4642 (1000) |
| 5 `stale` | `editor runtime: moving a mesh marks the navigation bake stale; moving it back clears the flag` | 71 (40) |
| 6 `docs` | `openspec validate implement-issue-28-navigation-authoring --strict`; `python3 tools/issue28_acceptance.py --check-docs` | exit 0 |

- **Red mutations through the ledger.** Each was applied, the tree rebuilt where it is C++, and
  `python3 tools/issue28_acceptance.py --criterion <key>` run: it printed `UNVERIFIED` for the named
  probes and `verified 0/1 selected criteria` with exit 1. The source was then restored with
  `git checkout -- <file>` (or the saved copy for an uncommitted file), rebuilt, and the same command
  printed `verified 1/1 selected criteria`.

| # | Criterion | Mutation | Red probes and failing assertion |
|---|---|---|---|
| L1 | 1 `bake` | `navigation_service.cpp` `read_settings`: `settings.cell_size = reader.read_f32() * 1.01F;` | both probes. test_navigation_service.cpp `CHECK_EQ(baked.tiles[index].digest, expected)` and `CHECK_EQ(nav::mesh_tile_digest(*mesh, slot), expected)` (4 tiles each), `CHECK_EQ(baked.fingerprint, fingerprint)`, `CHECK_EQ(baked.identity, ...)`; test_nav_runtime.cpp lines 303, 308, 313, 315, the same four checks (runtime case: 10 of 74 assertions failed) |
| L2 | 2 `overlay` | `nav_overlay.cpp` `NavCanvasSink::polygon` returns early on every second call | both probes. test_nav_overlay.cpp:175 `CHECK_LE(coverage.mismatched, coverage.expected / 100U)` (2178 vs 43); test_nav_overlay.cpp:207 `CHECK_GE(right_drawn.hit + (right_drawn.expected / 50U), right_drawn.expected)` |
| L3a | 3 `obstacle` | `navigation_service.cpp` `sync_obstacles`: `wanted.clear();` after the seam's obstacles are gathered | service probe. test_navigation_service.cpp:372 and :392 `CHECK_EQ(marked, usize{1})`, :374 `CHECK((!blocked.found \|\| blocked.partial))`, :385 the direct `find_path` check (4 of 1195 failed) |
| L3b | 3 `obstacle` | `navmesh.rs` `navigation.component.remove` replaces `remove_from(...)` with `let removed = false;` | MCP probe (exit 101). a_session_over_the_wire.rs:3615 `the engine received the removal before the second path query` |
| L4 | 4 `history` | `navmesh_service.rs` `take_completed` returns `self.unrecorded.clone()` instead of `take()` | MCP undo and redo: a_session_over_the_wire.rs:3456 `undo restores the unbaked identity` (left 3125673985, right 0); one bake transaction: navmesh_service.rs:819 `a completion is recorded once` (left 3, right 2). The projection, settings and desktop-parity probes stay green, as they do not bake |
| L5 | 5 `stale` | `navigation_service.cpp` `status` encodes the saved fingerprint as the current one (`encode_status(session, world, context, saved, saved)`) | both probes. test_navigation_service.cpp:415 `CHECK(stale.stale)`, :416 `CHECK_NE(stale.current, baked.fingerprint)`, :426 `CHECK(resized.stale)`; test_nav_runtime.cpp:344 `CHECK(runtime.stale(baked))` |
| L6a | 6 `docs` | spec delta `specs/navigation/spec.md`: both scenarios of `Stale bake detection` deleted | strict OpenSpec: `✗ [ERROR] navigation/spec.md: ADDED "Stale bake detection" must include at least one scenario` |
| L6b | 6 `docs` | `editor/README.md`: `### The Navigation panel` renamed to `### Navigation panel` | documentation: `missing: editor/README.md: ### The Navigation panel`, `issue #28 documentation: incomplete` |

The first attempt at L6a deleted only one of the requirement's two scenarios. Strict validation
still passed, because the requirement kept a scenario, so that mutation is not a red check. The
recorded L6a deletes both.

A first attempt at L5 left `current` unused, and `-Werror` refused the build. The runner then
reported green from the binaries of the previous build. A mutation only counts when its build
succeeds, and the recorded L5 builds.

## 1. Editor bake equals `build_tile`, tile by tile

- **Probes:** `editor_backend: navigation bake equals build_tile tile by tile` (service over a
  fixture seam) and `editor runtime: baking the test map equals build_tile tile by tile` (runtime
  seam over a `.cyworld` map).
- **Status:** verified. Red mutations M1 (engine), S1 (service), R1a and R1b (runtime), and L1
  through the ledger.

## 2. Overlay walkable area matches the mesh (image test)

- **Probes:** `nav overlay covers the projected walkable polygons` and `nav overlay per-world toggle
  draws only the enabled world` (CPU canvas, no device).
- **Status:** verified. Red mutations R2, R7, and L2 through the ledger.

## 3. Obstacle through the editor blocks and restores a path

- **Probes:** `editor_backend: an obstacle added through the service blocks the path and removing it
  restores it`, and the MCP wire test `navigation_obstacle_add_and_remove_reach_the_engine_over_mcp`.
- **Status:** verified. Red mutations S2, E7, and L3a and L3b through the ledger.

## 4. Undo/redo and MCP parity for bake, settings and component edits

- **Probes:** the MCP undo/redo and projection tests, the bake and settings transaction tests, and
  the desktop-versus-MCP history test (P10).
- **Status:** verified. Red mutations E1, E2, E8, E9, P10, and L4 through the ledger.

## 5. Stale bake detected after a geometry edit

- **Probes:** `editor_backend: navigation status reports a stale bake after a source change` and
  `editor runtime: moving a mesh marks the navigation bake stale; moving it back clears the flag`.
- **Status:** verified. Red mutations M8b (engine), S3 (service), R3a and R3b (runtime), and L5
  through the ledger.

## 6. OpenSpec change validated with `--strict`, and docs

- **Probes:** `openspec validate implement-issue-28-navigation-authoring --strict`, and
  `python3 tools/issue28_acceptance.py --check-docs`. The docs check requires the change's
  artefacts, the runtime test map, `docs/guides/navigation.md` (whose editor feature map lists
  Navigation as Implemented), and the navigation sections of `editor/README.md`,
  `samples/05b-editor-window/README.md`, `src/navigation/README.md`,
  `src/editor_backend/README.md`, `tools/build/README.md` and `docs/guides/README.md`.
- **Status:** verified. Red mutations L6a and L6b.

## Runner self-checks

`python3 tools/test_issue28_acceptance.py` (8 tests), also run in CI by `just quality-issue28-ledger`
in the `quality` job, which needs no build:

- a Cargo filter that selects no tests is not reported as passed;
- a native probe below its assertion floor, or one that executed no case, is not reported as passed;
- a failing command is not passed;
- every criterion has probes and a recorded red mutation, and every native probe runs with
  `--no-skip`, has a floor and contains no comma (doctest splits filters on commas);
- the docs check names each missing line, and the shipped docs are complete.

| # | Mutation (`tools/issue28_acceptance.py`) | Failing test |
|---|---|---|
| G1 | the empty-filter guard `if executed == 0:` becomes `if executed < 0:` | `test_an_empty_cargo_filter_is_not_passed`: `(True, 'passed') != (False, 'Cargo filter selected no tests')` |
| G2 | the floor guard `if count < probe.min_assertions:` becomes `if count < 0:` | `test_a_native_probe_below_its_assertion_floor_is_not_passed`: `(True, 'passed') != (False, 'only 2 assertions; needs 10')` |

Both were restored from a saved copy, and the 8 tests pass again.

## Cognitive complexity (task 6.3)

Measured on the functions this change adds or edits, against the backend target of 15 and the
frontend target of 8 to 12:

- **C++** (`clang-tidy readability-function-cognitive-complexity` over the compile database, every
  non-test `.cpp` the change touches): no changed function is above 15. The highest are
  `path_query` (15) in `navigation_service.cpp`, `tiles_overlapping` (12) in `bake.cpp`, and
  `dirty_regions`, `worlds`, `gather` (11 each) in `nav_runtime.cpp`, and `place_new_obstacles` and
  `status` (11 each) in `navigation_service.cpp`. The `navmesh.cpp` functions above 15
  (`rebuild_links` 31, `connect_within` 29, `find_nearest` 23, `closest_on_triangle` 19,
  `connect_tile` 16) are unchanged from main; the change adds one accessor to that file.
- **Rust** (`clippy::cognitive_complexity` with the threshold set per run): no function in
  `navmesh.rs`, `navmesh_service.rs` or `nav_bake.rs` is above 12, and no function in
  `panels/navigation_baking.rs` is above 8. `Editor::pump`, which gained the navigation settle call,
  scores 13.
- **Python** (`complexipy`): `missing_docs` 8, `main` 7, `probe_result` 7; everything else is
  lower.
