# Navigation authoring

A walkthrough of the editor's navigation baking tool (issue #28): what the editor does and what
the engine does, how to bake a navmesh and check it from the Navigation panel or over MCP, and
which tests prove each acceptance criterion.

**Governed by**: [`navigation`](../../openspec/specs/navigation/spec.md) and the change
[`implement-issue-28-navigation-authoring`](../../openspec/changes/implement-issue-28-navigation-authoring/proposal.md),
which also changes `editor-architecture` and `editor-viewport-and-gizmos`. The module READMEs linked
below are the detailed reference.

## Editor feature map

| Feature | Status | Where |
|---|---|---|
| Navigation | Implemented | the Navigation panel (`editor-navigation-baking`), eighteen `navigation.*` commands, the engine's `NavigationService` and the runtime overlay |

Before issue #28 the editor had no navigation tool. `Domain::NavigationBaking` now opens as a form
editor, and the navigation data drawable in the viewport is the engine's per-world overlay rather
than a debug view mode.

## Who does what

The editor is a client. It never bakes, hashes geometry, unprojects or casts rays.

| Where | What |
|---|---|
| [`src/navigation/`](../../src/navigation/README.md) | `bake_tiles`, `rebake_tiles`, tile digests, `source_fingerprint`, the `.cynavmesh` codec |
| [`src/editor_backend/`](../../src/editor_backend/README.md) | `NavigationService` (the `navigation.*` operations) and `CompositeEditorService`, which routes them beside the material and VFX operations |
| [`tools/build/`](../../tools/build/README.md) | The `navmesh` cook producer, which verifies a saved bake and puts it in the cook identity |
| [`samples/05b-editor-window/runtime/`](../../samples/05b-editor-window/README.md) | The host seam over the authored world (`nav_runtime.*`) and the overlay drawn into the frame (`nav_overlay.*`) |
| [`editor/`](../../editor/README.md) | The `navigation.*` commands, the `NavmeshService` client and the Navigation panel |

Navigation state is scene-document data. A `NavigationWorld` component carries the agent profile,
the build settings, the overlay flags and the accepted bake's identity. `NavMeshSurface`,
`NavObstacle`, `NavArea` and `NavLink` are ordinary components. Every edit, including a finished
bake, is one undoable transaction.

## Baking a navmesh in the editor

1. Start the engine and the editor on the sample project:
   `just run-editor-live --project samples/05b-editor-window/project --world worlds/city.cyworld`.
2. Open the **Navigation** tab (it sits beside Terrain in the scene-editing workspace) and press
   **Create navigation world**.
3. Under **Components**, press **Add Nav Mesh Surface** and set its bounds in the Inspector so
   they cover the ground. Meshes inside the surface are the bake's source geometry.
4. Adjust **Agent and build settings** (radius, height, max slope, step height, cell size, tile
   size, voxeliser) and press **Apply settings**. One apply is one undo entry.
5. Press **Bake**. The progress bar counts tiles as the engine reports them. When the bake
   completes, the panel shows the tile, polygon and link counts, and the viewport draws the
   walkable polygons.
6. Move a mesh and press **Check for changes**. The badge reads *Stale* until you bake again.
7. Under **Test path**, press **Pick start** and **Pick end** and click the viewport each time.
   The engine resolves each click onto the navmesh and draws the path. Add an obstacle over the
   path to see it route around, or block it.

Undo a bake with **Edit → Undo**. The world goes back to the previous bake identity, and the runtime
reloads that bake's saved `.cynavmesh` sidecar. Undoing the first bake leaves the world unbaked in
the engine too: the runtime sends `navigation.clear`, and path, flow-field and pick queries answer
`navigation.world.unbaked` until you redo or bake again.

### Sidecars belong in version control

The document records only the bake's identity; the navmesh itself is the content-addressed
`navigation/<identity>.cynavmesh` next to the project. Commit those files with the world. A clone
without them reopens with the badge reading *Sidecar missing: bake again to rebuild the navmesh*
(`navigation.bake.status` reports `sidecar_missing=true` and `engine_baked=false`), and the engine
holds no mesh for the world until someone bakes again.

### Choosing a voxeliser

Both back ends bake multi-tile worlds whose paths cross tile seams. **Recast** voxelises each tile
with a border of `agent radius + 3` cells, so its erosion sees the ground past the tile edge and
the polygons meet the neighbour tile's. **Engine** is the engine's own voxeliser. The two give
different polygons for the same inputs; a bake records which one it used, and changing it makes
the bake stale.

### Costs are painted with volumes

Area cost is authored with `NavArea` volumes: triangles whose centroid lies inside a volume take its
area, and the volume's cost multiplies path cost across them. Painting cost directly onto
polygons with a brush is not supported. Place and resize volumes instead.

## The same session over MCP

The MCP tools are the registered commands, so an agent runs the same steps:

```text
navigation.world.create
navigation.settings.set   values="agent_radius=0.25; tile_size=8; backend=engine"
navigation.surface.add    values="bounds.min=-16, -1, -16; bounds.max=16, 4, 16"
navigation.obstacle.add   values="shape.offset=4, 0, 4; shape.radius=1"
navigation.overlay.set    overlays="polygons, obstacles"
navigation.bake
navigation.bake.status    refresh=true
navigation.path.query     start=[0,0,0] end=[12,0,12]
edit.undo
```

`navigation.bake`, `navigation.path.query`, `navigation.flowfield.query` and
`navigation.point.pick` return a request id. Their answers appear in `navigation.bake.status`.

## Acceptance evidence

`python3 tools/issue28_acceptance.py` runs one set of probes per acceptance criterion. It passes
only when every probe passes:

- native cases run with `--no-skip` and must reach an assertion floor;
- a Cargo filter must run at least one test;
- a red mutation must be recorded for the criterion.

The mutations and the assertions they break are listed in the change's
[verification ledger](../../openspec/changes/implement-issue-28-navigation-authoring/verification.md).
The runner builds the probed test binaries in `build/dev` before it runs them, so a probe never
reports on a stale binary (`--no-build` skips that). `just quality-issue28-ledger` runs the runner's
own unit tests and the documentation check, and needs no build. CI enforces the rest: the `test`
job (linux-x86_64) runs `just quality-issue28-native` (the runner with `--native-only`) after
`just test-all`, whether or not that suite passed,
and the `editor` job's `cargo test` runs the Cargo probes' tests.

| Criterion | Probes |
|---|---|
| A bake equals `build_tile`, tile by tile | `integration.editor_backend_navigation`, `integration.editor_window_navigation` |
| The overlay covers the walkable polygons | `integration.editor_window_navigation` (CPU canvas, no device) |
| An obstacle blocks a path and removing it restores it | `integration.editor_backend_navigation`, the MCP wire test |
| Undo, redo and MCP parity | `cy-editor-mcp`, `cy-editor-services` and `cy-editor-shell` tests |
| A stale bake is detected | `integration.editor_backend_navigation`, `integration.editor_window_navigation` |
| OpenSpec and docs | `openspec validate --strict`, `issue28_acceptance.py --check-docs` |

## What is not built yet

- Navigation data is not a debug view mode. The overlay is a per-world toggle drawn on top of the
  lit frame, without a depth test.
- The Inspector edits the Nav* components undoably but has no MCP equivalent of its own. Agents use
  the `navigation.*.set` commands.
- Cost painting with a brush. Costs come from `NavArea` volumes only.
- The cook does not declare a `navmesh` node for a world with a `NavigationWorld` on its own: a
  project's build graph names the node by hand, and the game runtime does not yet load the cooked
  `.cynavmesh` into a game world. See the change's `tasks.md` (deferred items).
