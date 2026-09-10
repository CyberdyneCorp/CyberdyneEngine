# `src/camera/` — layer 4

**CyberCamera's world half**: the six things `camera-system`'s Seed left, at the layer that can reach
the world.

**Governed by**: `camera-system`, which reaches **Working** at M8.b — task 7.3.

## Why there are two camera directories, and which one to read first

`src/servers/camera/` (layer 2, M4) is the camera system: the four separated concepts, rig graphs and
their compiler, follow/orbit/offset/look-at/lens/noise/constraint/collision evaluation, the lens
model, the stack and its blends, cuts, the listener anchor, the streaming source, render views, the
impulse bus and the batched query protocol. **Read that module first.** Its README lists what it
deliberately left:

> Absent, not stubbed: framing and composition constraints, camera volumes, the strategy camera, the
> director camera, aim assistance and screen/world projection. All are `camera-system`'s and all are
> M8's.

This module is those six, and each of them is here rather than there because it needs something
layer 2 cannot name:

| what | needs |
|---|---|
| `framing.h` — multi-target framing, composition, dead zone, headroom | a target resolved from the world; cell-relative positions |
| `volume.h` — camera volumes, their blend rule and their index | a spatial index |
| `strategy.h` — the strategy camera: one zoom parameter, terrain following, map bounds, edge scroll | terrain height |
| `assist.h` — aim assistance and the director | gameplay spatial queries |
| `projection.h` — screen ray, world-to-screen, rectangle-to-frustum | `cy::render::ViewDescription` |
| `authoring.h` — CyberGraph as the rig's front end | `cy::graph` |

It **extends `namespace cy::camera`** and reuses `TargetBinding`, `TargetSample`, `Lens`,
`EvaluatedCamera` and `CollisionParams` from the server rather than declaring a second set. Two
definitions of a camera's target would be exactly the conflation `camera-system`'s first requirement
spends a page forbidding.

## What each header holds, and the requirement it answers

| file | requirement |
|---|---|
| `framing.h` | "Framing and composition" — one implementation for strategy selections, boss encounters, dialogue, fighting cameras and editor previews |
| `volume.h` | "Camera volumes" — priority, blend distance, and **found through a spatial index rather than by testing every volume each frame** |
| `strategy.h` | "Strategy camera" — zoom as ONE normalised parameter through curves; terrain height rather than per-frame casts; edge scrolling from a pointer action |
| `assist.h` | "Aim assistance" — a modifier between input and intent, observable in diagnostics; and the director's shot scoring |
| `projection.h` | "Screen and world projection" — accounting for the viewport rectangle, with no renderer internals |
| `authoring.h` | `visual-scripting`'s shared authoring layer, adopted by the camera |

## Four things worth knowing before changing anything

**The volume index is measured, not asserted.** `VolumeSet::tested()` reports how many volumes the
last query actually compared against, and `tests/test_framing.cpp` asserts it stays at or below four
over a set of two hundred. A comment claiming a spatial index would be a comment; this is a number
that fails when somebody replaces the grid with a loop.

**Framing reports when it could not honour a minimum screen size.** Two targets far enough apart
cannot both fill their declared fraction of the view. Keeping both visible wins — a framing that
drops a target has failed at the thing it is for — and `FramingSolution::screen_size_yielded` says
so, rather than the designer's minimum being silently ignored.

**The rig node vocabulary is `rig.*` and the collision with `camera.*` is deliberate.**
`cy::graph::camera::register_camera_nodes()` (M8.b task 2.4) registers the EXPRESSION vocabulary of
the shared SSA core — `camera.add`, `camera.smooth_half_life`, `camera.lens`, `camera.output`. The
rig vocabulary here is a different language at a different altitude and two of its names would
collide in one `NodeRegistry`, so it is `rig.target`, `rig.follow`, `rig.output`. Both can be
registered in one registry and an author can tell from the name which language they are writing in.

**There are two camera rig compilers in the tree, and that is a decision this module did not take.**
`cy::camera::compile()` (M4, `src/servers/camera/`) is what evaluates cameras today.
`cy::graph::camera::compile_rig()` (M8.b task 2.4) lowers a rig through the shared expression core
and is what `design.md` §1.7's table names for `camera-system`. They are not the same program and
neither is dead code: the first is a node-kind interpreter over a compact op list with no
allocation, the second is a hash-consed expression DAG with a declared phase boundary. Reconciling
them means either porting the M4 rig nodes onto the expression core — which the core's own
`Select`-is-not-lazy property makes awkward for the `Constraint` and `Collision` nodes — or deleting
the expression lowering for cameras. **Both have cooked data implications and neither is a change to
make quietly**, so this module bridges CyberGraph to the compiler that is actually running and the
choice is recorded here for the milestone's gate.

## Testing

`unit.camera_world` — framing, constraints, volumes, projection, the zoom curve, edge scrolling,
assistance and the director. No world, no device, no compiler.

`integration.camera_authoring` — authors a CyberGraph rig graph, validates it with the shared
validator, converts it to a definition, compiles it and drives a `CameraServer`. It runs two
compilers, which is why it is not in the unit tier.
