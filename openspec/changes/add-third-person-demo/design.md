# Design: a rendered third-person demo

## The game is Swift; the host is plumbing

Three behaviours on three level nodes. `Hero` owns the capsule (`CharacterController`), the
animator and the footsteps; `CameraDirector` owns the look and the framing; `Ambience` owns the
wind and the birds. They meet over one component the director writes on its own node — its yaw —
which the hero reads to turn the stick into the camera's frame. Two components go to the host: the
hero's facing, because the capsule has none and the ABI has no entry that turns a node, and the
report. The host reads the facing to place the hero's node and never acts on the report.

## The camera request becomes the orbit's intent

`Camera.setTarget` carries an absolute yaw, pitch and distance; the camera server's orbit takes
look deltas and one normalised zoom. `CameraAdapter` records the request as the rig's framing and
leaves the conversion to the host (its header says why). The host applies the difference between
the request and the last one it applied, so a request that does not change is a zero delta, and
maps the distance onto the orbit's zoom between its near and far. A request with no blend is a cut,
and a cut resets the rig — the collision's recovery included — so the director cuts once.

## The resolver, and the question the rig asks

`CameraCollisionResolver` sits in `cy::game_backend`, above both the camera server (layer 2, which
may not name a physics world) and the physics server: it reads the batch, sweeps a sphere of the
query's radius for a collision and casts a ray for an occlusion, and submits the answers. A probe
with no radius is a ray. A sweep that starts inside something answers zero.

The rig's collision node now publishes its queries about the position the rig WANTS — the frame's
position before any response moved it. Asked about the pulled-in position, the next answer was
"clear", the camera eased back into the wall, and the answer after that found it again. And the
`PullIn` response keeps an engaged flag from the first answer that pulls the camera in until the
recovery has carried it back out: a clear answer while it is engaged eases the camera out by the
recovery half-life rather than doing nothing (a pop), and the first contact snaps to the allowed
distance rather than easing from a smoother that was never set.

## The platform that was a stair

`CharacterController::update_ground` measures a moving platform as the ground body's displacement
since the previous step. It compared the ground body against `state_.ground_body` after
overwriting it with the new one, so the comparison always held, and stepping from one body onto
another measured the distance between their origins: four metres from the terrain to the first
stair tread, a 230 m/s "platform". The previous ground body is now read before it is replaced.

## Deterministic adaptive probes

`ScreenProbeGather::place` (the host oracle) keeps the first `capacity` adaptive candidates in
cell order. The device's placement took a slot per candidate with `InterlockedAdd` and refused past
the capacity, so while candidates fit the set was the same, and once they did not, the refused set
was the order the threads arrived in. The placement now only marks candidates; a second dispatch of
one 64-lane workgroup counts each lane's run of cells, scans the counts, and hands out slots in cell
order, writing the probes as the placement did. The counter still counts every candidate, so
readers' `min(counter, capacity)` is unchanged. Untimed: its interval is the trace's.

## The frame

The RTS test map's renderer with three changes: the temporal upscaler at Quality (every
screen-space stage at the render extent, the sampler's LOD bias), the sun's shadow map centred on
the hero, and a sky dome — the engine's frame draws no background, and the frame's materials carry
one emission each, so the dome is bands of elevation with the sky's radiance in each, and a disc for
the sun.

## The budget

16.7 ms a frame at 1920x1080 on the RTX 5060, recorded, submitted and waited for, averaged over the
run's frames that write no image, without the validation layer. The test asserts it on the machine
that has the device; CI has none and skips the drawn run.
