# `src/rendering/motion_blur/` — layer 4: motion blur on the device

`cy::rendering-motion-blur`. **Governed by** `rendering-post-processing` — "Motion blur" — and it is
the first consumer of `temporal-rendering`'s per-object motion vectors other than the temporal
resolve.

**Status: built and run on Vulkan, in the Development and Debug profiles.** `unit.`,
`integration.rendering_motion_blur` and `render.motion_blur` pass, every image case has been proven
red by a mutation (the list is `tools/roadmap/requirements-coverage.toml`'s "Motion blur" note), and
`docs/design/images/motion-blur-beauty-{off,on}.png` are the beauty shot's turntable frame without
and with it. Metal: the MSL is generated and embedded and has not run on a device from this change.

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
COPIED — an early return in the gather. So a closed shutter, and every still region of a moving
frame, is the frame without the stage byte for byte; `render.motion_blur`'s case (d) holds it to
that. The early return is a saving and not the guarantee: with it removed, every tap of a still
neighbourhood lands on the pixel itself and the weighted sum of one colour is that colour exactly,
which is why removing it leaves (d) green. What (d) does catch is a closed shutter that still blurs
at all (a tenth of a frame changes 15 428 texels).

## Where the motion comes from

The prepass velocity target, which since this module's change carries each instance's own motion:
`pipeline::FrameBindings` keeps the instance rows it uploaded last frame
(`pipeline/instance_history.h`), writes each row's previous placement after the current rows, and
`cy/frame.slang`'s depth vertex pushes the vertex through its previous placement and — for a mesh
deformed on the device — its previous vertices, which `DrawGeometry::previous_vertex_offset` names
in the other half of a double-buffered output such as `skinning::SkinnedBuffers`. Nothing asks a
system for its motion.

## The sky

A texel the prepass left cleared (reversed-Z depth 0) is the sky: its motion is the far plane's
reprojection, which is the camera's rotation alone. Its view depth is clamped to a finite
`kSkyViewDepth`, because under `perspective_reversed_z_infinite` the far plane `m32 / m22` divides
by zero and two sky texels would order as `inf - inf`; `integration.rendering_motion_blur`'s `a turning
camera blurs the sky under an infinite projection to finite colour` is the regression case.

## The suites

| Suite | Kind | What it proves |
|---|---|---|
| `unit.rendering_motion_blur` | unit | `pipeline::InstanceHistory`: rows kept, rebased to a moved camera, discarded on a cut or a recycled slot |
| `integration.rendering_motion_blur` | integration | the shutter arithmetic and the constants; the host reference over synthetic frames — how far a bar's blur reaches is the shutter's fraction of its motion, the background beyond it keeps every stripe, a still foreground over a moving background stays sharp, the camera and object scales separate in both directions, a closed shutter copies every pixel bit for bit, a turning camera blurs the sky to finite colour. Integration, not unit: the reference filters a 192x24 frame with fifteen taps a pixel, one to two milliseconds a case, and the unit budget is one |
| `render.motion_blur` | render | `pipeline_test::FrameScene` with a box sliding across it, on Vulkan: (a) every pixel's motion vector against the reprojection of the surface under it — the box's through its own displacement, the still world's through the camera's alone, with the camera still and moving; (b) the temporal resolve ghosts less behind the box than with camera motion only; (c) the blur's reach follows the shutter angle, the background beyond it is untouched, and the device's gather is the host reference's; (d) a closed shutter is the frame without the stage, byte for byte, and an open one is not; (e) with neither per-object motion nor the stage the frame is byte-identical to references drawn with the frame shaders from before this change — a moving box, and a still scene with per-object motion on; (f) a mesh deformed on the device moves by its stored previous vertices |

### How the streak is measured

A physical shutter integrates coverage, so its streak is a linear ramp as long as the distance the
edge travelled while it was open. McGuire's reconstruction is not a linear ramp: a nearer surface
covers a farther one along a cone, so the profile ahead of a moving edge falls off faster than
linearly and its 10 %-to-90 % width says little about the streak. The suites measure how far the
blur REACHES past the edge (`motion_measure.h`'s `smear_reach`: the last texel moved at least 1 %
of the way to the object's colour). Measured on the device, over a box moving 27.9 px a frame: 5 px
at 180 degrees and 11 px at 360, against half-motions of 7.0 and 14.0 — never past the shutter,
short of it by the cone's last percent. On the host, 2, 5 and 10 px against 3, 6 and 12.

### What (b)'s margin is

The scene is flat-shaded, so the temporal resolve's neighbourhood clamp already removes most of
camera-only motion's ghost; what remains in either run is mostly the edges of surfaces the box has
just uncovered, whose history is new. Per-object motion leaves 0.0080 of the energy against 0.0122
with camera motion only, and the case requires less than 0.8 of it. With the prepass's previous rows
ignored the two runs are the same frame.

### The readback copy

A pass created with `MotionBlurPassDescription::readback` reads back the gather's inputs and output
for the suites. The colour input is the temporal history, an image the frame assembly creates for
sampling and drawing only and whose next import it derives from how its own passes left it, so it is
never transferred out of: `motion_blur_copy.slang` samples it into a texture of the pass's own and
that is what is copied. The first version transferred directly and produced 129 validation errors a
run.

### Two faults this module's first run found outside it

- **The post-process sampled the wrong descriptor set after any compute stage.** A Vulkan descriptor
  set binds to the bind point of the last pipeline the command buffer bound, and
  `pipeline/frame_recorder.cpp` bound the temporal and post-process passes' sets before their
  pipelines. With motion blur's dispatches between them the sets went to the compute bind point and
  the resolve drew with the temporal pass's set, so the frame never showed the blur and (d)'s
  byte-identity held vacuously. `bind_frame_sets` now binds the pass's pipeline first; (d)'s
  open-shutter control is the regression case. `samples/12-beauty`'s resolve had the same order and
  crashed in the validation layer under `--motion-blur`.
- **`CY_CHECK_NEAR`'s tolerance is a relative epsilon** (`doctest::Approx::epsilon`), not an absolute
  one, so the tolerances this module's first draft passed it (2 to 3) accepted any value. These
  suites no longer use it for anything but small tolerances; the macro itself is unchanged.

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
