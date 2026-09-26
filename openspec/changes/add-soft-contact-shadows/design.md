# Design

## Soft shadows: PCSS sized by the light, not by a parameter

Three steps over the directional map, in `cy/shadow.slang`: a blocker search over a disc for texels
nearer the light than the receiver, a penumbra radius from similar triangles, and a filter over a
disc of that radius. For a directional light of angular radius `a`, a blocker `d` metres above the
receiver casts a penumbra of radius `d * tan(a)`; in the map's units that is
`(blocker - receiver) * depth_range * tan(a) / width`, so the light contributes one number,
`penumbra_per_depth`, derived by `make_pcss_shape` from its angular radius and the map's footprint.
There is no softness parameter. The kernel is clamped between one texel — so a contact is filtered
like the old 3x3 — and 24 texels, which also bounds the search.

Taps are Hammersley points spread over the disc and rotated per pixel by interleaved gradient noise.
A receiver the search finds no blocker for returns exactly one, so an open plane has no penumbra
and no noise; a receiver whose every tap is blocked returns exactly zero.

The map is read through `IShadowDepthSource`: the frame's is the bindless copy the shadow pass writes,
the beauty shot's a depth texture loaded without comparison. `soft_shadows.cpp` transcribes the
three steps, and the host suite measures the penumbra's width against the blocker's height and the
light's radius on an analytic half-plane.

Quality is `ShadowQuality`, which drives the tap counts as it already drives
`shadow_sample_count`: the budget's `FilterQuality` lever.

## Contact shadows: a screen-space trace, added to the map's answer

From each pixel's reconstructed position, lifted off its surface by one pixel's footprint along the
prepass normal, `steps` taps march toward the light over `length` metres. A tap that projects behind
the depth buffer's surface by less than `thickness` is inside an occluder; an occluder in the first
half of the trace is full shadow and one in the second half fades out. The lift is what makes an
open plane trace to exactly one: without it a tap near the start lands on the receiver's own surface
at a pixel centre, and at a grazing view half a pixel of rounding is more depth than the tap has
climbed.

Selection is per pixel: a pixel beyond `max_distance` or facing away from the light is not traced.
`apply_refinement` scales `max_distance` by `ShadowLever::Refinement`'s value, so the budget removes
refinement from the far receivers first and at zero traces nothing while the map is untouched.

The frame takes the darker of the map's visibility and the trace's: the trace cannot see what is
off screen or behind the nearest surface, and the map cannot see what is smaller than a texel.

## Where it sits in the frame

`FramePassKind::ContactShadows` is one of stage 4's screen-space passes, after the prepass and
before the opaque pass that reads it. It is declared by `ContactShadowPass` through
`FrameStageDeclaration`, and unlike ambient occlusion it has no single-pass fallback: a frame asked
for it without a producer and a target refuses to build. The target is persistent and imported
because the opaque pass samples it through a texture-table slot.

`cy/frame.slang` appends two fields to the frame block. With both flags clear the directional
shadow is the 3x3 filter it always was, and the contact term is `min(v, 1)` — which is `v` — so the
frame with the setting off is byte-identical to the frame before the change; `render.soft_shadows`
pins it to a reference rendered with the pre-change SPIR-V.

## Backends

Vulkan is built and tested. The trace's MSL is generated and committed with the depth bound as
`DepthTexture2D`, and the frame's MSL is regenerated; both are compiled by Slang only on this host.
D3D12: every entry compiles to DXIL through `just build-shaders`, but no DXIL is embedded.
