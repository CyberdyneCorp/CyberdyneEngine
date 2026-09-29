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

## 1. Editor bake equals `build_tile`, tile by tile

- **Probes:**
  - `editor_backend: navigation bake equals build_tile tile by tile` (service over a fixture seam).
  - `editor runtime: baking the test map equals build_tile tile by tile` (runtime seam over a `.cyworld` map).
- **Planned mutation:** make `bake_tiles` skip the last tile coordinate, or perturb `cell_size` in the service's settings decode. The coordinate-set or digest assertion must fail.
- **Status:** the engine-level probe is green and has a recorded red mutation (M1 above). The service and runtime probes are still open.

## 2. Overlay walkable area matches the mesh (image test)

- **Probe:** `nav overlay covers the projected walkable polygons` (CPU canvas, no device).
- **Planned mutation:** make the sink skip every second polygon. The covered-pixel count must fall outside the tolerance.
- **Status:** open.

## 3. Obstacle through the editor blocks and restores a path

- **Probes:**
  - `editor_backend: an obstacle added through the service blocks the path and removing it restores it`.
  - The MCP wire test `navigation obstacle add and remove reach the engine over mcp`.
- **Planned mutation:** make the service ignore `NavObstacle` entries from the seam. The blocked-path assertion must fail.
- **Status:** open.

## 4. Undo/redo and MCP parity for bake, settings and component edits

- **Probes:** cargo tests covering:
  - navigation settings, component and bake edits undoing and redoing over MCP;
  - desktop and MCP navigation histories agreeing;
  - command unit tests.
- **Planned mutation:** record the bake transaction twice (or not at all) on Completed. The history-length assertion must fail.
- **Status:** open.

## 5. Stale bake detected after a geometry edit

- **Probes:**
  - `editor_backend: navigation status reports a stale bake after a source change`.
  - `editor runtime: moving a mesh marks the navigation bake stale`.
- **Planned mutation:** leave the source vertices out of `source_fingerprint`. The stale assertion must fail.
- **Status:** open.

## 6. OpenSpec change validated with `--strict`, and docs

- **Probe:** `openspec validate implement-issue-28-navigation-authoring --strict`, plus a docs check that the touched READMEs and the editor feature map mention the navigation editor.
- **Planned mutation:** remove the Scenario from one requirement in a spec delta. Strict validation must fail.
- **Status:** open.

## Runner self-checks

- **Planned checks:** `tools/test_issue28_acceptance.py` checks the following:
  - a Cargo filter that selects no tests is not reported as passed;
  - a native probe below its assertion floor is not reported as passed.
- **Planned mutation:** remove each guard. The matching unit test must fail.
