# Derive per-object motion vectors and add motion blur to the frame

## Why

`temporal-rendering` asks for motion vectors "derived from data the renderer already holds: current
and previous instance transforms in the GPU scene, current and previous poses", for every moving
surface "without per-system effort". The frame's depth prepass derived CAMERA motion only: every
instance was taken to be where it is now, so a moving object's motion vector was the still world's,
and the temporal resolve reprojected its history from where it is rather than where it was.
`derive_surface_motion()` had no caller, and the requirements map recorded the requirement as
`exempt:m11e` with that finding.

`rendering-post-processing` asks for motion blur by "a tile-based maximum-velocity approach ...
gather along the dominant direction with depth-aware weighting", parameterised by shutter angle
with separate per-object and camera scaling. The tree had `motion_blur_length` and
`motion_blur_weight` — the arithmetic of the two scenarios — and no pass: no pixel was ever
blurred.

## What changes

- `pipeline::FrameBindings` keeps the instance rows it uploaded last frame
  (`pipeline/instance_history.h`), rebases them to this frame's camera, and writes each row's
  previous placement after the current rows in the one instance buffer. A stable identity per row
  (`FrameUpload::instance_ids`) and a history cut discard what is not the same instance's history.
- `cy/frame.slang` appends `motionControl` to the frame block after `volumetricFogControl` (544 to
  560 bytes). The depth vertex pushes each vertex through its previous placement and through its
  PREVIOUS POSITION — a third vertex binding, which is the position stream again for a rigid mesh
  and, for a mesh deformed on the device, the other half of its double-buffered output, named by the
  new `DrawGeometry::previous_vertex_offset`. With no previous rows the expression is the one the
  prepass always evaluated. The frame's committed SPIR-V and MSL are regenerated.
- A new module, `src/rendering/motion_blur/`: tile max, neighbour max and a McGuire-style gather,
  with committed SPIR-V and MSL, a host reference written expression for expression, and a
  persistent target. The shutter angle is `360 * shutter_seconds / frame_seconds` from the same
  camera controls that set the exposure.
- `ForwardFrame` gains a `MotionBlur` stage — step 8, after the temporal resolve and before bloom —
  declared by its producer through `FrameStageDeclaration`; `ScreenSpaceStageInputs` gains the
  velocity and the colour the chain has reached. `FrameAssembly` passes it through
  (`AssemblyView::motion_blur`, `FrameSinks::motion_blur`).
- Tests: `unit.rendering_motion_blur` (the instance history), `integration.rendering_motion_blur`
  (the host reference) and `render.motion_blur`, each image case proven red by a mutation.
- `pipeline::FrameRecorder` binds each pass's pipeline before its descriptor sets, which a compute
  stage between two graphics passes needs on Vulkan.

## Scope

Motion vectors for virtual geometry's clusters, mesh particles, sprites and world-space interface
are not written — each is drawn by a pass of its own rather than by the prepass — and the skinning
pass is not yet drawn through the frame recorder, so the deformed-mesh path is exercised by a mesh
whose two vertex halves the suite writes. Temporal diagnostics' visualisation of the vectors is not
built. The requirements map records each against its requirement.
