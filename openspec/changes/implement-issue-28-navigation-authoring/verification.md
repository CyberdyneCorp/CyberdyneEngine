# Verification ledger

This ledger records the executable evidence for issue #28. It is incomplete until every acceptance criterion has a green check and a recorded red mutation.

- `python3 tools/issue28_acceptance.py --list` lists the criteria.
- `python3 tools/issue28_acceptance.py` runs their probes and exits nonzero while any criterion is unverified.
- Native probes run with `--no-skip`. Each has an assertion-count floor, so that a skip on a null device cannot pass.
- The mutations below are the planned red checks. Each entry is filled in with the command, the failing assertion and the restoration once it has been executed.

## 1. Editor bake equals `build_tile`, tile by tile

- **Probes:**
  - `editor_backend: navigation bake equals build_tile tile by tile` (service over a fixture seam).
  - `editor runtime: baking the test map equals build_tile tile by tile` (runtime seam over a `.cyworld` map).
- **Planned mutation:** make `bake_tiles` skip the last tile coordinate, or perturb `cell_size` in the service's settings decode. The coordinate-set or digest assertion must fail.
- **Status:** open.

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
