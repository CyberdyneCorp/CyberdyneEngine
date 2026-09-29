# Proposal: Physics authoring tools in the editor

## Why

Issue #29 lists three physics tools the RTS needs in the editor: a physics debug view in the
viewport, joint and constraint authoring with gizmos, and ragdoll profile setup. The engine side
exists (#8): `PhysicsServer::debug_draw` emits colliders, contacts, constraint anchors and limits,
sleep state, velocities, centres of mass and broad-phase bounds into a sink, Jolt implements all ten
`ConstraintType`s, and `physics::ragdoll::Profile::generate` builds a profile from a skeleton. None
of it reaches an author:

- The viewport lists "Physics colliders" in `PLANNED_VIEWS`, as a view the engine cannot draw.
- A `cy::physics::Joint` holds runtime body handles, so no world file can describe one. The play
  session reads bodies and colliders from the `.cyworld` and nothing else.
- The editor cannot load a skeleton: model import stops before step 7 (import skeletons).

## What Changes

- **Physics debug layers.** `cy_editor_viewport::physics_view::PhysicsLayer` mirrors
  `cy::physics::DebugDrawFlags` bit for bit (a test reads `debug.h`). A viewport's layers travel as
  a trailing, optional field of the gizmo intent. The editor runtime draws them from the play
  session's physics world with `PhysicsServer::debug_draw`, projected through the frame's own view
  and rasterised into the published frame beside the gizmo. `viewport.physics.<layer>` and
  `viewport.physics.hide-all` toggle them as read commands. "Physics colliders" leaves
  `PLANNED_VIEWS`.
- **Joints in the world file.** A `Joint` component on body A's node names body B by entity
  reference and carries the kind, anchor, axis, range, swing, six-axis limits, motor, ratio, break
  thresholds and collide-connected flag. `cy::gameplay::PlaySession` resolves it after the bodies
  exist, derives frame B so the anchors coincide in the authored pose, and hands it to the bridge.
- **Entity references survive a save.** Both `.cyworld` writers now write an entity value as the
  referenced node's position (or `-`), and both readers resolve it to that node's identity on load.
  Before, the writer emitted the 64-bit identity and the engine's reader parsed it as a signed
  integer, so any reference to a node whose identity had its top bit set could not be read back.
- **Joint authoring.** `physics.joint.add`, `physics.joint.set` (one field, one transaction) and
  `physics.joint.remove` are reversible commands, so each is an MCP tool and `edit.undo` covers it.
  A change the engine would refuse at play is refused when it is made.
- **Joint gizmos.** `cy::physics::debug_draw_constraint`, moved out of the Jolt backend, draws a
  constraint's anchors, connection and limits from a description and two frames. The editor runtime
  draws the selected node's authored joint through it while editing, with its axis, so a joint looks
  the same before play and during it.
- **The physics panel** (`physics` panel kind): the layer toggles, the selected entity's joint with
  only the fields its kind reads, and the ragdoll limitation stated in the scaffold's diagnostics
  area. It uses the scaffold's header and parity check.
- **Ragdoll profiles are scoped, not built.** Without skeleton import there is nothing to generate
  a profile from. The panel says so; the coverage map records the exemption.

## Capabilities

### Modified Capabilities

- `editor-viewport-and-gizmos`: physics debug layers are requested per viewport and drawn by the
  engine; the selected entity's joint is drawn by the engine as a gizmo.
- `physics`: joints can be authored in a world file and are created at play.

## Impact

Engine: `src/scene/serialization` (entity reference encoding), `src/servers/physics` (the shared
constraint drawing), `src/backends/physics-jolt` (uses it), `src/servers/render` (the intent field),
`src/gameplay/play` (authored joints), `samples/05b-editor-window/runtime` (drawing). Editor:
`cy-editor-viewport`, `cy-editor-services` (joints, viewport commands, gizmo request, world file),
`cy-editor-shell` (the panel), `cy-editor-interface` (the panel kind), tests in `cy-editor-mcp`.
The gizmo intent gains a trailing field that older peers ignore or default.
