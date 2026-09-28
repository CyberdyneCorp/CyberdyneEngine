# `src/rendering/lightmaps/` — layer 4

A baked lightmap in the forward frame: `lightmap_bake::BakedLightmap`'s planes as textures the
forward pass samples at its ambient term, the view-block words that describe them, and the one rule
— read off `gi::exclusion_for()` — for which ambient source a surface takes.

**Governed by**: `rendering-global-illumination` — "Lightmap baking". OpenSpec change
`add-lightmap-baking`. Issue #36, stage 4.

| No lightmap: the flat ambient | The corner's lightmap |
|---|---|
| ![Off](../../../docs/design/images/lightmaps-off.png) | ![On](../../../docs/design/images/lightmaps-on.png) |

`render.light_probes`' corner — a red wall, a white back wall and two white cubes on a white floor
under one 100 000 lux sun — baked by `lightmap_bake::bake_lightmaps` from the same boxes. The back
wall and the cubes' fronts face the camera and receive no direct light, so what they show is the
ambient term. With the lightmap the back wall is lit by the sunlit floor and turns red beside the
red wall, and the cube beside it does too.

The bake behind the right-hand picture: six boxes, one 256 x 256 page, 10 649 texels traced at 24
samples and one bounce, 8 881 more filled by dilation, about 0.5 million rays, in 0.9 s on one core
of the development machine; 512 KiB on the device as irradiance (Rgba16Sfloat), 1 MiB directional,
1.5 MiB SH L1. The atlas itself, first plane, cropped to the rows the corner uses:

![The corner's atlas](../../../docs/design/images/lightmaps-atlas.png)

## Three modules, one seam each

| Module | Owns | Knows nothing of |
|---|---|---|
| `cy::rendering-lightmap-bake` | the atlas, the bake, the encodings, the cooked payload, `sample_lightmap` | a device |
| `cy::rendering-pipeline` (`FrameViewData::lightmap_*`, `GeometrySource::lightmap_uvs`, `cy/frame.slang`) | the frame words, the fourth vertex stream and `lightmapAmbient` | GI |
| **this module** | the planes' textures and upload, `write_lightmaps`, `frame_ambient_source` | how a texel was traced |

`lightmapAmbient` is a transcription of `lightmap_bake::atlas_coordinate` and `sample_lightmap`;
`render.lightmaps` (b) holds the frame to the host sampler over every pixel of the corner.

## What a caller does

1. Bake (or load a cooked `lightmap`, `decode_lightmap_asset`) and `LightmapTextures::upload` it
   outside the device frame. An unchanged lightmap is not copied again.
2. Put `textures.slot(plane, n)` for each plane in the frame's set 0 texture table and call
   `write_lightmaps(slots, lightmap, mode, upload.view)` before the frame's upload. Under a mode
   `exclusion_for()` says excludes lightmaps (`Probe`, `Dynamic`, `None`) it writes nothing.
3. Give each lightmapped draw its rectangle: `DrawSurface::gi_address = lightmap.addresses[i]`.
4. Put the meshes' cooked `TexCoords2` in `GeometrySource::lightmap_uvs`, laid out like UV0.

A draw whose address is zero is lit as before, and a frame with no lightmap, or one no draw
addresses, is byte-identical to the frame drawn before lightmaps existed: `render.lightmaps` (c)
holds it to `references/lightmaps_absent.png`, a copy of `render.light_probes`' reference drawn by
the frame shader of 0f1dfd1.

## Which ambient source

The frame's ambient is ONE source, where the resolve's is a confidence-weighted mean of several: the
lightmap and the volume each already hold the sky and the bounces. `frame_ambient_source` takes the
highest-priority source `exclusion_for()` admits — lightmap, volume, flat sky — and `render.lightmaps`
(d) binds a volume beside the lightmap and requires every lightmapped pixel unchanged and the one
unlightmapped cube to take the volume.

## What is not here

The shadow mask: the bake writes it for every stationary light (`lightmap_bake/README.md`), but
this module neither uploads it nor has the frame read it, so a stationary light's direct term is
still shaded unshadowed by the lightmap path. Mip levels (the atlas's gutter is laid out for them,
the upload is one level); a streamed atlas.
