# `src/rendering/motion_blur/` — layer 4: motion blur on the device

`cy::rendering-motion-blur`. **Governed by** `rendering-post-processing` — "Motion blur" — and it is
the first consumer of `temporal-rendering`'s per-object motion vectors other than the temporal
resolve.

## What it does

Three compute dispatches over the prepass velocity target, at the frame's own
`FramePassKind::MotionBlur` stage — step 8 of the chain, after the temporal resolve and before
bloom:

```
velocity + depth ──► tile max ──► neighbour max ─┐
temporal colour + velocity + depth ──────────────┴─► gather ──► target ──► bloom / exposure
```

| Stage | Shader | What it computes |
|---|---|---|
| tile max | `shaders/motion_blur_tile_max.slang` | every `max_radius_pixels`-square tile's longest blur vector — "compute per-tile maximum velocity" |
| neighbour max | `shaders/motion_blur_neighbour_max.slang` | the longest over each tile's 3x3 neighbourhood: the dominant direction a pixel can be reached from |
| gather | `shaders/motion_blur_gather.slang` | McGuire et al.'s reconstruction filter (I3D 2012) along that direction: a nearer tap covers the pixel as far as its own streak reaches, a farther one only as far as the pixel's streak reveals it — "depth-aware weighting so background does not smear over foreground" |

`include/cy/rendering/motion_blur/motion_blur.h` holds the settings, the 128-byte push-constant
block all three read, and a host reference of all three written expression for expression in the
shaders' order. `motion_blur_pass.h` is the device half: the pipelines, the persistent target, and
the `FrameStageDeclaration` the frame reaches it through.

## The one physical number

A shutter open for a fraction `f` of the frame interval integrates `f` of the frame's motion, so a
surface moving `m` pixels a frame leaves a streak `f * m` long. The shutter ANGLE is that fraction
in degrees — `360 * shutter_seconds / frame_seconds` — and `settings_for_camera` takes it from the
same `CameraControls::shutter_seconds` the exposure is computed from. 180 degrees is half a frame of
motion (`motion_blur_length`, `src/rendering/post/`, is the arithmetic, and `make_motion_blur_constants`
calls it rather than restating it). The streak is centred on the frame's instant, so the blur
vector every stage stores is half of it, in pixels.

`camera_scale` and `object_scale` are the requirement's separate per-object and camera scaling. The
camera's share of a pixel's motion is where last frame's camera saw the pixel's own depth — the same
reprojection the prepass derives camera motion with — and the remainder is the object's. At 1 and 1,
the physical camera, nothing is separated and the pixel's motion is used whole.

## The free parameters

| Setting | Default | Why it exists |
|---|---|---|
| `max_radius_pixels` | 24 | the longest blur radius and the tile edge. A cost bound: the 3x3 neighbourhood reaches one tile, so no pixel can be reached from farther. A longer motion is clamped to it. |
| `samples` | 15 | gather positions per pixel, the middle one the pixel itself. A quality bound on the line integral the streak is. |
| `soft_depth_metres` | 0.05 | two taps closer in depth than this are one surface. A tolerance for depth precision and surface thickness, the role `ContactShadowSettings::thickness` plays. |

## What a closed shutter is

Every tile's maximum is zero, and a pixel whose neighbourhood blurs less than half a pixel is
COPIED — an early return in the gather, not a weighted sum that happens to come out equal. So a
closed shutter, and every still region of a moving frame, is the frame without the stage byte for
byte; `render.motion_blur`'s case (d) holds it to that.

## Where the motion comes from

The prepass velocity target, which since this module's change carries each instance's own motion:
`pipeline::FrameBindings` keeps the instance rows it uploaded last frame
(`pipeline/instance_history.h`), writes each row's previous placement after the current rows, and
`cy/frame.slang`'s depth vertex pushes the vertex through its previous placement and — for a mesh
deformed on the device — its previous vertices, which `DrawGeometry::previous_vertex_offset` names
in the other half of a double-buffered output such as `skinning::SkinnedBuffers`. Nothing asks a
system for its motion.

## The suites

| Suite | Kind | What it proves |
|---|---|---|
| `unit.rendering_motion_blur` | unit | the shutter arithmetic and the constants; the host reference over synthetic frames — a bar's streak is the shutter's fraction of its motion, the background beyond it keeps every stripe, a still foreground over a moving background stays sharp, the camera and object scales separate, a closed shutter copies every pixel bit for bit; and `pipeline::InstanceHistory` — rows kept, rebased to a moved camera, discarded on a cut or a recycled slot |
| `render.motion_blur` | render | `pipeline_test::FrameScene` with a box sliding across it, on Vulkan: (a) every pixel's motion vector against the reprojection of the surface under it — the box's through its own displacement, the still world's through the camera's alone, with the camera still and moving; (b) the temporal resolve ghosts less behind the box than with camera motion only; (c) the streak follows the shutter angle, the background beyond it is untouched, and the device's gather is the host reference's; (d) a closed shutter is the frame without the stage, byte for byte; (e) with neither per-object motion nor the stage the frame is byte-identical to references drawn with the frame shaders from before this change — a moving box, and a still scene with per-object motion on; (f) a mesh deformed on the device moves by its stored previous vertices |

## Regenerating the shaders

```
python3 src/rendering/motion_blur/shaders/regenerate.py --slangc build/dev/Development/bin/slangc
```

The SPIR-V and MSL are committed, for the reason `src/rendering/occlusion/` gives. D3D12: every
entry compiles to DXIL through `just build-shaders --strict`, and no DXIL is embedded.

## What is not here

Motion vectors for virtual geometry's clusters, mesh particles, sprites and world-space interface
are not written: the prepass draws the frame's instances, and those four are drawn by passes of
their own. `tools/roadmap/requirements-coverage.toml` records each against `temporal-rendering`'s
"Motion vectors are derived, not authored".
