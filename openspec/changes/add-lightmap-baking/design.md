# Design

## One rectangle per object, one word per draw

The importer unwraps each mesh into its own unit square (xatlas, one atlas per mesh). A level places
many instances of those meshes, so the packer places one RECTANGLE per receiving instance and the
mesh's UV2 square is mapped into the rectangle's interior by a scale and an offset. One mesh's UV2
stream is then shared by every instance of it, which is what lets the frame read the cooked
`TexCoords2` stream unchanged. Re-running xatlas over the whole level would have packed charts more
tightly and given every instance its own UV2 — a vertex buffer per instance.

Rectangles sit on a grid of `page_size / 128` texel blocks, so a rectangle is four seven-bit numbers
and a page: exactly one 32-bit word, `GpuDrawInstance::gi_address`, with page + 1 in the top nibble
so that zero, the field's default, is "not lightmapped". No draw needs a second record and the
shader needs no second buffer. The cost is that a rectangle is rounded up to a block.

A gutter of `max(2, 2^mips)` texels surrounds each interior, where `mips` is clamped to
`log2(block)`: below that a rectangle boundary stops being a texel boundary at some mip level. A
bilinear tap at the interior's edge, at any declared level, reads the object's own dilated border.

## The bake is `gi::PathTracer`'s

There is no second path tracer. `MeshSceneTracer` is the acceleration structure — a static
`cy::Bvh` over every placed triangle — behind `SceneTracer` and `Occluder`, and the path tracer's
hits resolve their materials through `gi::GiScene` surface cards built from the same triangles, as
the real-time path's do. Alpha testing (a mask at UV0) and transparency (a hashed stop with the
surface's opacity, so shadow rays transmit on average) are decided at the hit.

`gi::PathTracer` bounces with `albedo * irradiance` — the surface cache's convention — where a
Lambertian surface leaves `albedo * irradiance / pi`, which is the frame's (and `BoxProxyScene`'s).
The bake hands the path tracer its lights divided by pi, which makes every bounce the frame's
without changing the tracer; the seeds use the caller's lights, in the caches' own convention.

## What a texel holds

The frame's ambient term is `albedo * ambient radiance`, the mean incoming radiance over the cosine
hemisphere. A texel stores that — for the placed lights' direct term too only when the content is
`DirectAndIndirect`, because the frame shades every light it has dynamically until light mobility
exists.

- **Irradiance**: `E(n_g) / pi`. One RGBA plane.
- **Directional**: the same, plus `v`, the luminance-weighted mean incoming direction scaled by its
  directionality, and `w = 1 + v . n_g`. A shading normal reads `rgb * max(0, 1 + v . n) / w`:
  exact at the geometric normal, brighter toward the light. Two planes.
- **SH L1**: the same fit per channel, `a_c + b_c . n`, so a normal tilted toward a coloured wall
  turns the colour too. Three planes.

The trace accumulates linear moments (the mean, luminance times direction, each channel times
direction), and the denoiser, the dilation and the seam solve all operate on the moments; the planes
are encoded last and rounded through half precision, so the host sampler reads exactly what the
device does.

## Denoising, dilation and seams

The denoiser runs once per moment channel over the whole stacked atlas, with no history (an atlas
is one frame) and the chart id as the hard boundary: two charts adjacent in the atlas are strangers
in the world. Dilation then fills every rectangle to its edge from its own texels only. Seams are
edges two charts of one object share in space with smooth normals and different UV2 (a hard edge's
two sides are lit differently on purpose and are left alone); both sides' bilinear footprints are
moved to their common value by a few Gauss-Seidel sweeps of the minimum-norm correction.

## Which ambient source the frame takes

The resolve's answer is a confidence-weighted mean of several sources; the frame's ambient is ONE,
because the lightmap and the volume each already hold the sky and the bounces. `frame_ambient_source`
takes the highest-priority source `exclusion_for()` admits — lightmap, then volume, then the flat
sky — and `write_lightmaps` switches the frame's lightmap on only for a mode that admits a lightmap
and excludes the volume for the surface that has one. `Probe` mode draws none.

That rule exposed `exclusion_for(Baked, has_lightmap)` excluding only the dynamic sources: a
lightmapped surface in a volume averaged the two. It now excludes the volume.

## The fourth vertex stream

The forward pipelines declare the lightmap coordinates as a fourth binding (`TEXCOORD1`, location 3)
and the recorder binds `GeometrySource::lightmap_uvs` there, or UV0 for a source without one. A draw
reads it only when its `gi_address` names a rectangle, so every caller that predates the stream
binds what it bound and draws what it drew — which `render.lightmaps` (c) checks against a reference
drawn by the frame shader before lightmaps existed.

## Incrementality

The `lightmap` build-graph node's key is its description, its upstream bundles' output digests and
the producer version: an unchanged level is served from the artefact store. Re-solving only the
region one moved object affects is the next slice's.
