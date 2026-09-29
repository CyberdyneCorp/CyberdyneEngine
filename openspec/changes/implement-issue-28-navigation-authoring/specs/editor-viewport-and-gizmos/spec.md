# editor-viewport-and-gizmos Spec Delta

## ADDED Requirements

### Requirement: Engine-drawn navigation overlay
The engine SHALL draw each navigation world's navmesh over the viewport frame when that world's overlay is enabled. The overlay SHALL use the engine's navigation debug overlays and SHALL show:
- walkable polygons in their area colours, with obstacle-marked polygons coloured by their effective area;
- tile borders;
- off-mesh links;
- obstacle footprints;
- the current test path and, when one is requested, flow-field arrows.

Incremental rebakes SHALL update the overlay on the next frame. The editor SHALL NOT rasterise navigation data itself.

#### Scenario: Overlay covers the walkable area
- **WHEN** the overlay draws a known navmesh onto a cleared canvas from a known view
- **THEN** the set of covered pixels SHALL match the projected walkable polygons within a stated tolerance

#### Scenario: Known map through the frame's overlay path
- **WHEN** the runtime bakes its known test map and draws an editor frame's overlays with the recorded overlay flags
- **THEN** each pixel inside one projected walkable polygon SHALL carry that polygon's area colour, and the ground, a painted area and an obstacle footprint SHALL show in their own colours

#### Scenario: Polygon partly behind the camera
- **WHEN** a walkable polygon has corners behind the camera
- **THEN** the overlay SHALL draw the part in front of the near plane rather than drop the polygon

#### Scenario: Per-world toggle
- **WHEN** the overlay is disabled for one of two navigation worlds
- **THEN** only the other world's navmesh SHALL be drawn

### Requirement: Navmesh point picking is engine-side
Test-path endpoints and NavLink endpoints SHALL be picked in the viewport by an engine operation. The operation SHALL resolve the pixel against the frame's view onto the navmesh and SHALL return the hit point and polygon, or a miss. The editor SHALL NOT unproject or raycast.

When the engine reports a path, the editor SHALL show its points, its cost, and whether it is partial.

#### Scenario: Two-point test path
- **WHEN** an author arms the test path and clicks two points on the navmesh
- **THEN** the editor SHALL show the engine's A* plus funnel path between the picked points, its cost, and whether it is partial

#### Scenario: Two-point link placement
- **WHEN** an author places a NavLink by clicking its start point and then its end point
- **THEN** one undoable transaction SHALL add the link with the picked endpoints
