# `src/rendering/depth_of_field/` — layer 4

Depth of field on the device: step 7 of `rendering-post-processing`'s chain. The lens is
`src/rendering/post/`'s thin-lens `circle_of_confusion`; this module is the five compute dispatches
that gather it into a frame.

**Governed by**: `rendering-post-processing` — "Depth of field", "Chain order and colour space".
OpenSpec change `add-depth-of-field`.

## The files

| File | What it holds |
|---|---|
| `focus.h` | `DofSettings`, `DofView`, `DofConstants` (the push block) and the host twin of every shader expression: the radius in pixels, the depth's inverse, the aperture's polygon, the gather's taps |
| `dof_pass.h` | `DepthOfFieldPass`: the five pipelines, the stage the frame declares through `FrameStageDeclaration`, and their record callbacks |
| `shaders/dof_common.slang` | the constants and the functions `focus.cpp` twins |
| `shaders/dof_setup.slang` | the half-resolution layer: colour and signed radius of each 2x2 block's nearest surface |
| `shaders/dof_tiles.slang` | the near and far fields' reach per tile, and its 3x3 dilation |
| `shaders/dof_gather.slang` | the far and near fields, scatter-as-gather, at half resolution |
| `shaders/dof_composite.slang` | the fields over the sharp image, at full resolution |
| `shaders/regenerate.py` | recompiles all five into `src/dof_spirv.h` and `src/dof_msl.h` |

## The frame

```
temporal ─► source ─┬─► setup ─► tiles ─► dilate ─┐
            depth ──┤     │                       ▼
                    │     └──────────────► gather (far, near)
                    │                             │
                    └─────────────────► composite ─► FrameResources::depth_of_field ─► bloom ─► post
```

`PostChainConfig::depth_of_field` is the setting. On, `ForwardFrame` declares
`FramePassKind::DepthOfField` after the temporal resolve and before bloom, creates the
full-resolution target every later stage reads, and hands the stage to this module
(`FrameSinks::depth_of_field`) with the colour the chain has reached as
`ScreenSpaceStageInputs::source`. Off, no pass and no target exist.

```cpp
pass.create(device, {width, height});
pass.set_settings(settings);                  // the lens and the gather's limits
// every frame
pass.set_view({projection, width, height});   // the matrix the depth was written with
sinks.depth_of_field = pass.stage();
```

## Five things worth knowing before changing anything here

**The radius is the lens's, not a tuning.** `circle_of_confusion` is a diameter on the sensor as a
fraction of its height, and the image is the sensor: the radius in pixels is half of it times the
image height, `K (d - F) / d`. `focal_length_for_field_of_view` gives the focal length the
projection's field of view implies, so a caller does not describe two different lenses.
`render.depth_of_field` fits a disc profile to a blurred far edge and holds its radius to the lens's.

**Two fields, and only one of them is normalised.** The far field is the background seen without
what is in front of it: gathered over the pixel's own radius, from far texels only, normalised. A
focused pixel has no radius, so the background never blurs onto it; a focused texel has no radius,
so it never reaches the background either. The near field is gathered over the dilated tiles' reach
and not normalised: each tap is weighted by the share of its texel's disc it stands for, so the sum
is the pixel's coverage by near-field discs, and the composite lays the foreground's blur over the
focused pixel by exactly that — the blur extends past the silhouette, as the scenario asks.

**The downsample keeps the nearest surface.** Each half-resolution texel takes its 2x2 block's most
negative radius and the colour of the texels within a pixel of it. A near silhouette is not thinned
by the downsample, and a focused silhouette in front of a blurred background stays focused, which
the far field's rule depends on. The far upsample is bilateral for the same reason.

**In focus is written as read.** A pixel whose radius is inside the one-pixel focus band and that no
near-field disc covers is written with the bits it was read with. The in-focus plane and a pinhole
(f/infinity, `K = 0`) are the stage's identity, and `render.depth_of_field` asserts both byte for
byte — the pinhole through the assembled frame against the frame without the stage.

**The tiles are as wide as the largest radius.** So a 3x3 block of tiles holds every near-field
texel whose disc can reach the centre tile's pixels. `max_radius_fraction` — 2 % of the image height
by default — is therefore a cost bound and not a look: a larger circle is clamped.

## The free parameters

| Parameter | Default | Why it exists |
|---|---|---|
| `max_radius_fraction` | 0.02 of the height | bounds the taps and sizes the tiles |
| `max_rings` | 8 (217 taps) | the gather's sampling density: a tap a texel out to eight half-resolution texels of radius |
| `blades` | 0 (a circle) | the aperture's shape: 5–16 straight blades inscribe that polygon |
| the focus band | 1 pixel of radius | the target's resolution limit; stated in `focus.h`, not a setting |

## What is not here

A custom aperture texture: the shape is a circle or a bladed polygon. A temporal accumulation of the
gather: its taps are a fixed pattern, and a still view is still. Occlusion within the far field: a
nearer far-field surface does not hide a farther one's blur, because the far field is gathered as
one layer. Autofocus is `post/effects.h`'s `track_focus`, the caller's to run; the beauty sample
focuses on a named point's distance along the view axis. D3D12: every entry point compiles to DXIL
(`just build-shaders`), and nothing embeds it, as for every device pass in the tree.

## What it looks like

`just capture-beauty-depth-of-field` photographs the beauty shot focused on the copper sphere and on
a far column at f/1.4 (`docs/design/images/depth-of-field-beauty-*.png`), and
`render.depth_of_field` writes its own frames beside the test binary (`depth-of-field-*.png`).
