# Verification ledger

This ledger records the executable evidence for issue #28. It is incomplete until every acceptance criterion has a green check and a recorded red mutation.

- `python3 tools/issue28_acceptance.py --list` lists the criteria.
- `python3 tools/issue28_acceptance.py` runs their probes and exits nonzero while any criterion is unverified.
- Native probes run with `--no-skip`. Each has an assertion-count floor, so that a skip on a null device cannot pass.
- The mutations below are the planned red checks. Each entry is filled in with the command, the failing assertion and the restoration once it has been executed.

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

## 1. Editor bake equals `build_tile`, tile by tile

- **Probes:**
  - `editor_backend: navigation bake equals build_tile tile by tile` (service over a fixture seam).
  - `editor runtime: baking the test map equals build_tile tile by tile` (runtime seam over a `.cyworld` map).
- **Planned mutation:** make `bake_tiles` skip the last tile coordinate, or perturb `cell_size` in the service's settings decode. The coordinate-set or digest assertion must fail.
- **Status:** the engine-level probe (M1), the service probe (S1) and the runtime probe (R1a, R1b) are green with recorded red mutations.

## 2. Overlay walkable area matches the mesh (image test)

- **Probe:** `nav overlay covers the projected walkable polygons` (CPU canvas, no device).
- **Mutation:** make the sink skip every second polygon. The covered-pixel count falls outside the tolerance (R2 above). The per-world toggle has its own mutation (R7).
- **Status:** green with a recorded red mutation.

## 3. Obstacle through the editor blocks and restores a path

- **Probes:**
  - `editor_backend: an obstacle added through the service blocks the path and removing it restores it`.
  - The MCP wire test `navigation obstacle add and remove reach the engine over mcp`.
- **Planned mutation:** make the service ignore `NavObstacle` entries from the seam. The blocked-path assertion must fail.
- **Status:** the service probe (S2) and the MCP wire probe (E7: the obstacle add and its removal reach the engine as synced component operations before each `navigation.path.query`) are green with recorded red mutations.

## 4. Undo/redo and MCP parity for bake, settings and component edits

- **Probes:** cargo tests covering:
  - navigation settings, component and bake edits undoing and redoing over MCP;
  - desktop and MCP navigation histories agreeing;
  - command unit tests.
- **Mutations:** record the bake transaction on every pump (E1) or not at all (E2), send the default settings (E8), or split a settings gesture into one transaction per field (E9). The history-length, identity or payload assertion fails in each case.
- **Status:** green with recorded red mutations (E1, E2, E8, E9 above). The desktop half of the parity
  check, where the panel's gestures and the same commands sent as an agent sends them record the same
  history and settings, is P10.

## 5. Stale bake detected after a geometry edit

- **Probes:**
  - `editor_backend: navigation status reports a stale bake after a source change`.
  - `editor runtime: moving a mesh marks the navigation bake stale`.
- **Planned mutation:** leave the source vertices out of `source_fingerprint`. The stale assertion must fail.
- **Status:** the engine fingerprint (M8b), the service probe (S3: status stops recomputing the fingerprint) and the runtime probe (R3a, R3b) are green with recorded red mutations.

## 6. OpenSpec change validated with `--strict`, and docs

- **Probe:** `openspec validate implement-issue-28-navigation-authoring --strict`, plus a docs check that the touched READMEs and the editor feature map mention the navigation editor.
- **Planned mutation:** remove the Scenario from one requirement in a spec delta. Strict validation must fail.
- **Status:** open.

## Runner self-checks

- **Planned checks:** `tools/test_issue28_acceptance.py` checks the following:
  - a Cargo filter that selects no tests is not reported as passed;
  - a native probe below its assertion floor is not reported as passed.
- **Planned mutation:** remove each guard. The matching unit test must fail.
