## ADDED Requirements

### Requirement: Lightmaps are baked into shared atlases by the offline path tracer
The engine SHALL pack every static, lightmap-receiving object of a level into shared atlas pages as
one rectangle per object, sized from the object's world area, the level's texel density and the
object's own resolution scale, with a gutter around each rectangle's interior sufficient for
bilinear filtering at every declared mip level. The bake SHALL path-trace each covered texel with
`gi::PathTracer` over the level's triangles for a configurable bounce count, with emissive surfaces
as light and alpha-tested and transparent occlusion; SHALL denoise with the shared denoiser, dilate
every rectangle to its edge and reconcile seams between charts; SHALL encode irradiance, directional
or SH L1 planes; and SHALL seed the dynamic caches from the same run. The cooked result SHALL be
content-keyed, so an unchanged level is not re-baked.

#### Scenario: Texels match the path tracer's ground truth
- **WHEN** a small room is baked and its texels are read back at points on its surfaces
- **THEN** they SHALL match an independent path-traced reference at those points within the stated
  relative error

#### Scenario: Normal maps still respond
- **WHEN** a directional or SH L1 lightmap is read with a shading normal tilted toward a bright
  surface
- **THEN** the result SHALL be brighter than at the geometric normal and nearer the reference at the
  tilted normal than the irradiance-only answer
- **AND** an irradiance-only lightmap SHALL return the same value for every shading normal

#### Scenario: Seams are not visible at the bake resolution
- **WHEN** two charts of one object, or two objects, meet along a smooth edge
- **THEN** bilinear reads either side of the seam SHALL agree to within one 8-bit step of a mid-grey
- **AND** every texel of the object's rectangle, gutter included, SHALL hold light

#### Scenario: A buried texel takes its own surface's light
- **WHEN** part of a chart is buried inside another object, next to another chart's padding
- **THEN** every buried texel SHALL be filled from its own chart, however wide the buried region
- **AND** a directional read there at the chart's own normal SHALL equal its irradiance

#### Scenario: An unchanged level is not re-baked
- **WHEN** a level whose description and meshes did not change is built again
- **THEN** the bake node SHALL be served from the cache with the same bytes, and no unwrap SHALL run

### Requirement: Lightmaps light the forward frame's ambient term
When a draw carries a lightmap address and the GI mode admits lightmaps, the forward pass SHALL take
that surface's ambient radiance from the lightmap, sampled through the cooked `TexCoords2` stream
and decoded by the lightmap's encoding, in place of the irradiance volume's or the flat ambient; a
surface without an address SHALL be lit as before. Which source a surface takes SHALL follow
`gi::exclusion_for()`. A frame with no lightmap bound, or bound and addressed by no draw, or bound
under a mode that excludes lightmaps, SHALL be byte-identical to the frame drawn before lightmaps
existed.

#### Scenario: A baked coloured wall bleeds onto nearby surfaces
- **WHEN** a lightmap baked with a sunlit red wall is bound
- **THEN** white surfaces near the wall SHALL be measurably redder than the same surfaces far from it

#### Scenario: The frame follows the host sampler
- **WHEN** the frame is drawn with ambient light only
- **THEN** its pixels SHALL correlate with the host's `sample_lightmap` prediction above 0.97 in
  brightness and redness

#### Scenario: A lightmapped surface in a volume takes the lightmap only
- **WHEN** a lightmap and an irradiance volume are bound together
- **THEN** a lightmapped surface SHALL be drawn exactly as with the lightmap alone
- **AND** a surface without a lightmap SHALL take the volume

### Requirement: The unwrap is cached by geometry
A reimport SHALL NOT re-run the UV2 unwrap when the mesh's geometry did not change: an unchanged
source SHALL be served by its derivation key, and a changed source whose geometry and unwrap options
are unchanged SHALL copy the previous unwrap, byte for byte.

#### Scenario: Unwrap is cached
- **WHEN** a mesh is reimported without geometry changes
- **THEN** the cached UV2 unwrap SHALL be reused and no unwrap SHALL run
