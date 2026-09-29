# Design: physics authoring tools (#29, Physics)

## Context

The editor is a client of the engine: it asks and displays, the engine owns the data and the
simulation, and every authoring action is a registered command inside a document transaction, so
the MCP tool list covers it. Wave 0 (`add-editor-specialised-scaffold`) gave specialised editors one
header, one diagnostics area and one parity check. The runtime the editor attaches to
(`cy_editor_window_runtime`) renders the world, composites the transform gizmo into the published
frame in software, and runs `cy::gameplay::PlaySession` for Play.

## Decisions

### Physics layers are overlays, not a view mode

`ViewMode` mirrors `cy::render::DebugViewMode`, which replaces the frame's shading. Physics
visualisation is drawn over whatever the shading is, and several layers compose, so it is a bit set
(`PhysicsOverlays`) beside the view mode rather than a twentieth mode. The bits are the engine's
`DebugDrawFlags` and a test reads the header, the same technique `viewmode` uses on the render
server's name table.

### The request rides the gizmo intent

The editor already sends a `GizmoIntent` continuously (one in flight) with the camera and the
viewport size. The layers are one more piece of intent about what to show, appended after
`game_camera` and optional on both decoders, so an older runtime ignores it and an older editor asks
for none. A new protocol message would have needed a second send path, a second sequence and the
same fields.

### The layers draw the simulated world

`debug_draw` walks a physics world, and a world exists while a session plays or is paused. The
layers are therefore a play-time view; while authoring, the selected entity's joint is drawn from the
authored world instead. Building a physics world just to outline colliders while authoring would be
a second simulation of the same world, and the authored collider is already the mesh-independent
shape the play session will create.

### One constraint drawing for Jolt and for authoring

The Jolt backend drew anchors, the connection and limits in anonymous functions. They move to
`cy::physics::debug_draw_constraint(description, frame_a, frame_b, sink)` unchanged (the Jolt
constraint debug cases, which count limit lines, still pass), and the runtime draws an authored
joint through it with frames computed exactly as play computes them. A joint therefore looks the
same selected in the editor and running in play. The authoring gizmo adds the axis, which the
simulated view does not need.

### A joint is authored on body A and names body B

`cy::physics::Joint` is a `ConstraintDescription` with runtime handles. The authored `Joint` holds
what a person sets: a kind, the node carrying body B as an entity reference (zero is the world), and
the joint frame as an anchor and an axis in body A's rotated, unscaled frame (a physics body has no
scale). Frame B is not authored: at play it is `inverse(B) * (A * frame_a)`, so the anchors coincide
in the authored pose, which is what placing two bodies and joining them means. One range pair
(`limit_min`, `limit_max`) serves as the hinge angle, slider travel, swing-twist twist and distance
span, because a joint has one kind. All eighteen fields are declared on every joint, as the collider
declares all four, so the schema does not depend on the first joint added.

### Joints are handed over after the bodies exist

`PlaySession::build` syncs the bridge, then reads every node's `Joint`, resolves both body handles
through `PhysicsBridge::body_of`, and adds the ECS `Joint`; a second sync creates the constraints.
An unknown kind, a node with no body, or a target with no body is counted in
`PlayReport::joints_refused` and skipped, as the bridge does with a body it refuses. A live edit that
rebuilds a body rejoins every joint that touches it, because the bridge destroys a constraint whose
body handles changed.

### Entity references are written as positions

A `.cyworld` has no stable per-node id: a node's identity is a hash of the document and its ordinal,
the ordinal is its file position plus one on load, and a save after a deletion renumbers the file.
Parent links are already written as positions for that reason. Entity values were written as
identities, which name nothing after such a save, and the engine's reader parsed the u64 word as an
i64, so a world with a reference to a node whose identity had its top bit set could not be read.
Both writers now emit the referenced node's position, or `-` for none or a node that is gone, and
both readers resolve a position to the identity the node there is given. In memory a reference is
always an identity, which is what an editor transaction carries to the runtime. The
`serialization-and-prefabs` requirement that references be stable local ids is met by the `.cydoc`
scene format, which has local ids; the `.cyworld` has none to key on, and this is the same rule its
parent links follow.

### `physics.joint.set` takes a field and a value

The command registry fills an omitted optional parameter with its default, so a command with one
parameter per field could not tell "not given" from "set to the default", and one call would write
all eighteen. `set` takes `field` and `value` as text, parses the value in the field's kind,
validates the whole joint as the engine's `validate` would, and writes only what changed, in one
transaction. A kind change to a distance joint whose range is free takes the default span in the
same transaction, because "free" is not a distance.

### The panel is not a seventeenth domain

`editor-architecture` enumerates sixteen specialised editors and the contract gate compares that
list with `Domain`. It names no physics editor, and joints and debug layers are viewport and entity
authoring rather than a new surface. The panel is its own kind (`physics`) and reuses the scaffold's
header, diagnostics area and parity check (`command_parity`, which `register_tool` now calls too),
so its commands are refused at start-up on the same terms as a specialised tool's.

### One gesture, one transaction

A dragged or typed field writes into `Inputs::physics_pending` and is committed as one
`physics.joint.set` when the drag is released or the field loses focus, so a drag is one undo entry.

## Scoped out

- **Ragdoll profile setup.** `Profile::generate` needs a finalized `animation::Skeleton`; the editor
  cannot import one (model import stops before step 7) and the engine has no skeleton asset format a
  runtime could load for it. The panel states this in its diagnostics area and the coverage map
  records it. Re-enter when skeleton import lands: generate through an engine service request, and
  edit per-bone shape, mass, limits and motors as one transaction per field.
- **Dragging joint handles in the viewport.** The engine draws the joint gizmo; anchors, axes and
  limits are edited in the panel. Direct manipulation needs gizmo handle kinds the layout protocol
  does not have.
- **Physics layers while authoring.** See "The layers draw the simulated world".

## Risks

- The layers are rasterised on the CPU like the gizmo, one segment at a time. A world of thousands
  of colliders costs frame time while the layer is on; the runtime reports the segment count.
- A reference to a node in another world is not expressible in a `.cyworld`; a joint to another
  world's body is refused as "no body" at play.
