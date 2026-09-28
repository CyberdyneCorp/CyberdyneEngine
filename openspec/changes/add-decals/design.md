# Design

## A decal is a cluster element, ranked before it is assigned

The lights' assignment already carried a `Decal` header in every cluster; nothing filled it. The
assembly now does, in the same `assign_clusters` call as the punctual lights, so "decals SHALL
participate in cluster assignment as a distinct element type" is one pass with one more element
type in it rather than a second pass beside the first.

What a list records is a decal's RANK in application order, not its index in the caller's array.
`assign_clusters` writes every list ascending, so a fragment that walks its list front to back
applies the decals in sort order — a later decal covers an earlier one — without sorting anything
per pixel. The rank comes from `decal_application_order`, which `DecalBudget::application_order`
now calls too: one ordering, ascending `sort_order` with ties on the decal's own `id`, total, so an
array handed over in another order ranks identically.

A decal's bound is its ORIENTED BOX — `rendering-forward-clustered` names "oriented box for decals"
— tested by separating axes (the cluster's three and the box's three) after the bounding-sphere test
that rejects most clusters for less. The nine edge-cross axes are left out, which makes the test
conservative: it can keep a cluster the box misses by a corner and can never drop one it touches.
A decal is thin, so the box matters: its sphere is mostly empty.

The element's layer mask is the decal's channels, so a decal whose channels are empty is in no list
at all and costs nothing. The shader tests a decal's channels against the receiver's
`GpuDrawInstance::layer_mask` too — which is how a decal restricted to characters stays off the
floor — and that word was never written before this change (see "Two defects").

## The table is words in a texture

The forward frame's view set is bound by five other committed shaders. A decal buffer as an eighth
binding would change that set for all of them, so the table rides the seam ambient occlusion,
contact shadows and the irradiance volume already use: a texture in the frame's bindless texture
table, named by one `uint4` appended to the view block.

`Rgba8Unorm`, one 32-bit word per texel, 1024 texels a row, sampled at texel centres. Eight-bit unorm
because every backend filters it with the frame's one linear sampler, and a texel-centre sample of an
8-bit channel returns `k / 255` exactly, so `round(x * 255)` recovers the byte and four of them the
word — floats included, bit for bit. A half-float table, which the irradiance volume uses, would
round a decal's centre to a centimetre at fifteen metres and every index past 2048.

Positions are CAMERA-RELATIVE, subtracted from the origin in double precision when packed, as
`build_gpu_light` does for a light. A record stores each box axis divided by its half-extent, so a
receiver's box coordinates are three dot products and "inside" is all three in [-1, 1].

The engine's frame reads a decal's rank from the cluster buffers its light loop reads, at the
`Decal` header. A shader with no cluster buffers — the beauty shot's, the world sample's — asks the
packer to copy the assembly's decal lists into the table with the grid and the view-depth row it
needs to find a pixel's cluster; `kDecalListsInTable` makes the engine's frame read that copy too,
which is how the suite holds the two to the same picture.

## Applied to the surface, before the light

`cy/decal.slang` takes the receiver's albedo, roughness, metallic, emission and shading normal and
returns them modified; the caller hands the result to its light loop. Per decal:

`amount = coverage(uv) × mask(uv) × angle fade × distance fade × edge fade`

then each channel is `lerp(receiver, decal, amount × weight)`. The angle and distance fades are
`decal_angle_fade` and `decal_distance_fade` transcribed. The edge fade softens the box's near and
far faces over `edge_softness` of its depth, so a box cutting a curved receiver leaves no line.

The normal is TILTED, never replaced: the decal's height is `coverage × relief_metres`, its gradient
in the decal's plane is taken by central differences in uv and carried to metres by the axes' own
scale, and the tangential part of that gradient is subtracted from the receiver's normal — surface-
gradient bump mapping, which keeps the receiver's own normal-map detail under the decal.
`blend_decal_normal`'s reoriented normal mapping is the answer for a decal with a tangent-space normal
map; this change's decals have a height, and the gradient is the exact answer for one.

Because all of it happens before the light loop, the decal is lit by the receiver's lights, in the
receiver's shadow, occluded by the receiver's ambient occlusion and fogged by the receiver's air.
`render.decals` (d) is the check: in the receiver's umbra a decalled pixel is byte-identical to the
same frame with the sun switched off.

## Free parameters

| Parameter | Value | Why it exists |
|---|---|---|
| fade angle | 60° default (`DecalInstance`) | the angle past which a projection stretches more than twice (1 / cos 60°) |
| distance fade | 20–40 m default; the world marker 3–4 km | so a decal nobody can see costs nothing; set past the view where it must not show |
| shape parameters | per material | authored: an outline is what an artist paints into a mask |
| edge softness | per material | the box's faces cut receivers; a hard face leaves a line |
| relief | 2 mm char, 8 mm moss | the mark's physical height |
| albedo | soot 0.03; moss (0.045, 0.085, 0.022); marker paint (0.80, 0.55, 0.06) | measured reflectances, linear |

## Two defects

- **`GpuDrawInstance::layer_mask` was never written.** `build_draw_list` left it at all ones, so
  every draw claimed every layer. `VisibleInstance` now carries the layer mask out of the broad
  phase — the layer test already loaded it — and the draw record copies it.
- **`apply_gpu_cull` dropped a survivor's flags.** It routed on the spatial flags (transparent,
  moved) and never stored them, so a device-culled draw reached `build_draw_list` with a flags word
  of zero — not skinned, not a shadow receiver — while `cull_view` carried them. Both are now
  carried, and a case holds the two paths to the same flags and layer mask.

## Samples

`samples/12-beauty` reads `decal-material` and `decal` lines from its `.cyshot`; with `--decals on`
the stage reserves a table texture at the material table's last slot, hands the decals to the
assembly, declares the table's upload into the frame's graph before the assembly declares the pass
that samples it, and stages the words — with the assembly's lists — once the frame is assembled.
`samples/10-world` gains `--ground-marker`: one ring decal where the camera looks, its table in a
fourth storage binding of the world's set 0 (empty until a frame writes one, which reads one word
and changes nothing). `just capture-decals` photographs both, off and on.
