# Design

## Ownership

The engine owns every navigation computation: voxelisation and tiling, tile digests, the geometry fingerprint, incremental rebakes, path and flow-field queries, navmesh picking and overlay rasterisation. The editor stores authored intent in the scene document: the settings, the Nav* components and the identity of the accepted bake. It sends backend-service requests and displays the results. This follows the route #17 used for material and VFX compilation (`src/editor_backend` services plus registry commands).

## Engine: bake API (`src/navigation`)

- `NavBakeSettings`: `NavBuildParams`, the `NavMesh` tile size, and an `AgentProfile`. The issue's settings map as follows:
  - radius maps to `agent_radius`;
  - height maps to `agent_height`;
  - max slope maps to `agent_max_slope_degrees`;
  - step height maps to `agent_max_climb`;
  - cell size maps to `cell_size`;
  - tile size maps to the `NavMesh` constructor argument;
  - backend maps to `NavBuildBackend` Recast or Engine. Automatic is not offered to authors, because the bake must name its back end.
- `NavBakeSource`: the geometry plus the surface volumes, the area volumes, the obstacles and the links, as the host gathers them for one navigation world.
- `bake_tiles(allocator, settings, source, region, NavMesh&, NavBakeObserver*) -> Expected<NavBakeReport>`:
  - covers every tile coordinate `(x, z)` whose XZ bounds `[x*t, (x+1)*t]` overlap `region`, taking the Y range from the geometry;
  - calls `build_tile` for each tile and then `NavMesh::add_tile`;
  - a tile with no walkable cells is counted as empty and is not an error;
  - the observer receives `(done, total, coord)` after each tile, and its return value requests cooperative cancellation;
  - a full bake also removes resident tiles outside the region, so the mesh holds exactly the region's non-empty tiles;
  - the report aggregates the `NavBuildReport` counters and lists the coordinates built, which the per-tile diagnostics use.
- `rebake_tiles(..., dirty Aabb, ...)` rebuilds only the tiles that overlap `dirty` and returns their coordinates. `add_tile` replaces each tile and relinks its neighbours, and tiles outside the region keep their slot salt.
- `tile_digest(const NavTileData&) -> u64` hashes the coordinate, the bounds, the vertices, the polygons (corner range, area, cost, centre) and the corners, in order. `NavBuildReport::duration_ns` is wall-clock time and is excluded. `mesh_tile_digest(const NavMesh&, slot)` computes the same value from a resident tile, so a baked mesh can be compared with a direct `build_tile` without copying.
- **Codec.** `encode_nav_bake` and `decode_nav_bake` handle a versioned `.cynavmesh` blob. It holds the header (magic, version, settings, backend, source fingerprint, bake identity) followed by each tile's data and digest. Decoding re-derives each digest and refuses a mismatch.
- `source_fingerprint(settings, source, producer_version) -> u64` hashes the vertices, the indices, the per-triangle layer, tag and area, the surface and area volumes, the settings and the back end. Obstacles and links are runtime overlays applied without a rebuild, so the fingerprint excludes them.
- **Bake identity.** `bake_identity = hash(source_fingerprint, ordered tile digests)`.

### Areas and cost painting

`NavArea` is authored as a volume. At build time, triangles whose centroid lies inside an area volume take that volume's `AreaType`. The latest-authored volume wins, and the order is stable by node identity. Editing a volume re-bakes only the tiles overlapping the union of its old and new bounds. `NavArea::cost` enters the navigation world's area-cost table (`NavAreaCosts`), which the test-path `PathFilter` uses. Painting cost directly onto polygons is out of scope. Cost is painted by placing and resizing area volumes.

### Obstacles

`NavObstacle` edits apply through `NavMesh::add_obstacle` and `remove_obstacle` with no voxel rebuild. Moving an obstacle removes the old footprint and adds the new one. The incremental update reports the tiles that overlap the old and new bounds as its affected tiles, and it leaves every other tile's digest and salt unchanged. An obstacle's centre is its node's world translation plus the shape offset, so the existing transform gizmo moves obstacles.

## Engine: editor service (`src/editor_backend`)

`NavigationService : abi::EditorServiceBackend` (header `include/cy/editor/navigation_service.h`). Its per-session state is one navigation context per navigation world: its `NavMesh`, the settings, the area costs, the last `NavBakeReport` and the saved bake identity and fingerprint. There is one pending request per session. Cancellation is cooperative. Each request ends in exactly one terminal event. Failure codes are stable strings (for example `navigation.bake.unavailable`, `navigation.busy`, `navigation.settings.invalid`, `navigation.world.unknown`).

Every operation uses schema 1.

| Operation | Effect | Result |
|---|---|---|
| `navigation.bake` | Full bake of a world from the host seam. It emits a PROGRESS event per tile across polls. | Report, per-tile digests, fingerprint, bake identity, sidecar path. |
| `navigation.status` | Recomputes the fingerprint from the seam and compares it with the saved one. | `baked`/`unbaked`, `stale`, identities, last diagnostics. |
| `navigation.update` | Incremental update for a dirty region, sent by the host after a document sync. | Affected tile coordinates. |
| `navigation.path.query` | `find_path` plus `straighten` with the world's filter. | Points, cost, `found`, `partial`, nodes expanded. |
| `navigation.flowfield.query` | `FlowField::build` toward a target over a region. | Cell grid with directions and reachability. |
| `navigation.point.pick` | Resolves (viewport, frame, x, y) on the navmesh through the host's pick view. | Hit point and polygon, or `miss`. |
| `navigation.overlay.set` | Sets the per-world overlay flags. | Acknowledgement. |

`capabilities.get` advertises these operations only when the host supplied a seam. Without a seam, each operation fails with `<op>.unavailable`.

**Host seam.** `NavigationSourceRuntime` is a pure virtual interface:
- `worlds()`;
- `gather(world, NavSourceBuffers&)`, which returns world-space triangles with per-triangle layer, tag and area, plus the surface and area volumes;
- `obstacles(world)` and `links(world)`;
- `store_bake(identity, bytes) -> path`;
- `load_bake(identity) -> bytes`;
- `pick_ray(viewport, frame, x, y) -> Ray`.

The engine service runs the tiling loop and owns the mesh, and the host only supplies data.

**Routing.** `CompositeEditorService` holds up to eight `(prefix, backend)` routes. It opens one child session per backend, forwards each `submit` and `cancel` by the longest matching operation prefix, polls every child in turn, and answers `capabilities.get` with the merged capability lists. `MaterialService` stays unchanged. The runtime currently polls once per request, so it gains a per-frame drain loop that forwards PROGRESS and terminal events.

## Persistence and cook identity

- **Saved bake.** A successful bake is saved as a content-addressed sidecar, `<project>/navigation/<bake_identity>.cynavmesh`, which the runtime host writes through `store_bake`. The world document records the navigation world's settings and, on its `NavigationWorld` component, `bake_identity`, `source_fingerprint`, `tile_count` and `sidecar`. The sidecar is never overwritten, because a different bake has a different identity. Undoing a bake therefore restores the previous identity, and the runtime reloads that sidecar with `load_bake` and redraws the overlay from it.
- **Stale detection.** `navigation.status` compares the saved `source_fingerprint` with the fingerprint the engine recomputes from the current sources. A geometry edit (a moved or edited MeshRenderer, or a changed surface or area volume) or a settings edit makes the bake stale. The editor displays the flag and never hashes geometry itself.
- **Cook.** The `navmesh` producer (`tools/build/src/navmesh_producer.cpp`, registered by appending to `add_content_producers`, with version `kNavmeshProducerVersion`) reads the referenced sidecar. It decodes it and verifies every tile digest and the stored identity, refuses a sidecar whose identity differs from the world's `bake_identity`, and writes the cooked asset. The node key covers the sidecar content digest and the producer version, so a rebake or a producer version bump changes the world's cook identity.

## Editor

**Document schema.** The components are declared by commands in `cy-editor-services/src/navmesh.rs`. Their names are the engine names without the namespace, pinned from both ends as `bodies.rs` does:
- `NavigationWorld`: `world`, agent radius, height, max slope, step height, cell size, tile size, back end, overlay flags, plus the bake identity fields;
- `NavMeshSurface`;
- `NavObstacle`;
- `NavArea`;
- `NavLink`.

The runtime reads them by name from the synced world (the `field_value` pattern). Engine reflection of the nav components is not required.

**Commands** are registered with one appended line in `builtin::register`, so the desktop, MCP and scripts share them:

| Class | Commands |
|---|---|
| ReversibleMutation | `navigation.world.create`, `navigation.settings.set`, `navigation.overlay.set`, `navigation.bake`, `navigation.surface.add/set`, `navigation.obstacle.add/set`, `navigation.area.add/set`, `navigation.link.add/set`, `navigation.component.remove` |
| Read | `navigation.settings.get`, `navigation.bake.status`, `navigation.path.query`, `navigation.flowfield.query`, `navigation.point.pick` |

Every mutation is one `with_transaction` per gesture.

**Asynchronous bake.** `navigation.bake` validates the settings, sends `navigation.bake` and returns the request id. A separate `NavmeshService` (`navmesh_service.rs`, with its own `accept`) decodes PROGRESS and terminal events. `Editor::pump` calls it with one appended line. On Completed, the editor records one transaction that sets the bake identity fields. On Failed, it records nothing and keeps the diagnostics, and `navigation.bake.status` exposes the progress, the diagnostics and `stale`.

**Panel.** A new `panels/navigation_baking.rs` reads the `NavmeshSettings` read model and pushes only `Intent::Invoke`. It has:
- a world picker;
- the profile and build settings;
- Bake with a progress bar and diagnostics;
- a stale badge;
- overlay toggles;
- add buttons for each component;
- pick-start and pick-end and flow-field controls for the test path.

It is wired by appending one entry each to:
- `can_open`;
- `panel_title` and `BUILT_IN_PANEL_KINDS`;
- the docking tabs;
- the `panels/mod.rs` module and dispatch arm, plus one `Inputs` field;
- the accessibility test list.

The file and module names use `navmesh` and `navigation_baking`, which keeps them apart from camera navigation (`cy-editor-viewport/src/navigation.rs`). Issue #29 owns `panels/*` and `specialised/*`, so the shared files receive only appended lines.

**Viewport modes.** An armed pick mode in the panel's inputs makes the next viewport click invoke `navigation.point.pick` instead of selection. The panel uses it for the test path's start and end points and for the two endpoints of a NavLink (the two-point gizmo). A link placement records one `navigation.link.add` transaction. An MCP client passes world points directly.

**Inspector.** Once the schema declares the Nav* types, the generated Inspector shows and edits their fields as undoable transactions. MCP parity for these edits comes from the dedicated `navigation.*.set` commands, not from the Inspector path.

## Viewport overlay

`samples/05b-editor-window/runtime/nav_overlay.{h,cpp}` implements `NavDebugSink` over the CPU `Canvas`:
- polygons are filled with their area colours, using the effective area so that obstacle-marked polygons show;
- tile borders, links and obstacle footprints are drawn with `project_to_pixel`;
- test paths are drawn as `path_segment` calls, and flow fields as arrows.

`draw_frame_overlays` draws it for each navigation world whose overlay flag is set. The overlay is composited over the lit frame and is not depth-tested. It is a per-world toggle, not a `DebugViewMode`, so the `PLANNED_VIEWS` "Navigation data" entry's note is updated to point at this overlay.

## Incremental updates

When a synced document change touches a NavObstacle, a NavArea, a NavMeshSurface or a MeshRenderer that lies inside a surface, the runtime computes a dirty region from the union of the old and new bounds and calls `navigation.update` in process. The overlay is redrawn on the next frame. Undo applies the reverse document change, so the same path restores the mesh.

## Risks

- **Two sessions own shared editor files.** This change and #29 both edit the same editor files, which conflict on array-length literals. The mitigation is append-only edits and a rebase before the PR.
- **Metal paths may lack a device.** CPU overlay tests need no device. Any Metal-backed test calls `native_frame_device()` and SKIPs with a reason when no usable device exists.
- **MSVC narrowing.** Tile coordinates use `i32`, and every conversion from `floor` to `u32` or from `u64` to `u32` is an explicit `static_cast`.
