## ADDED Requirements

### Requirement: The frame shades a lightmapped surface's baked lights through the lightmap
On a surface the frame draws with a lightmap, the frame SHALL shade each light whose direct term the
lightmap's texels already hold — every `Static` light — with no direct term, so that light is not
counted twice; SHALL shade each `Stationary` light's direct term at the frame's own intensity and
colour, attenuated by the light's baked shadow-mask channel, taking the darker of that channel and any
real-time shadow the frame has for the light; and SHALL shade every other light as it would without a
lightmap. The lightmap's shadow mask SHALL be uploaded beside its planes, and the lightmap SHALL name,
by the frame's own light order, which frame light each mask channel shadows and which frame lights'
direct terms it holds. A frame with no lightmap bound, or one that no draw addresses, SHALL be
byte-identical to the frame drawn before the shadow mask reached it.

#### Scenario: A stationary light's baked shadow holds at a new intensity
- **WHEN** a level is baked with a `Stationary` light and drawn with that light, and the light's
  intensity is then changed at run time without a rebake
- **THEN** pixels deep in the light's baked shadow SHALL be unchanged, byte for byte
- **AND** pixels in its full light SHALL change with the intensity
- **AND** when the frame's light is not matched to the bake's, the same shadowed pixels SHALL be lit
  and SHALL change with the intensity

#### Scenario: A stationary light takes the darker of its baked and real-time shadows
- **WHEN** a level is baked with a `Stationary` light and drawn with that light's real-time shadow map
  bound, and a movable object the bake never saw casts a shadow onto a lightmapped surface
- **THEN** the movable object's shadow SHALL darken the lightmapped surface
- **AND** every pixel SHALL be, channel for channel, the darker of the same frame drawn through the
  mask alone and the same frame drawn through the shadow map alone

#### Scenario: A lightmap the frame cannot describe is refused and changes nothing
- **WHEN** a light whose direct term the lightmap holds sits past the lights the frame can name
- **THEN** the lightmap SHALL be refused for that frame
- **AND** the frame's view data SHALL be left exactly as it was, with no lightmap switched on

#### Scenario: A static light is not counted twice
- **WHEN** a level is baked with a `Static` light and drawn with and without that light in the frame
- **THEN** every lightmapped pixel SHALL be the same in both frames
- **AND** a surface with no lightmap SHALL be lit by the light

#### Scenario: No lightmap is the frame as it was
- **WHEN** a frame is drawn with no lightmap bound, with one bound that no draw addresses, or with
  one bound under a mode that excludes lightmaps
- **THEN** it SHALL be byte-identical to the reference drawn by the frame shaders that predate the
  shadow mask on the same device

### Requirement: The uploaded lightmap carries a mip chain filtered per chart
The bake SHALL build every mip level below the base that the atlas's gutter and chart padding
protect, for the lightmap's planes and its shadow mask. Each coarse texel covered by a chart SHALL
take its value from that one chart's texels; each coarse padding texel SHALL belong to the chart
nearest it at the base resolution and take that chart's value; no coarse texel SHALL take a value
from outside its own rectangle. The cooked lightmap SHALL carry the chain, the upload SHALL carry
every level, and the frame SHALL sample the lightmap and the mask with the implicit level of detail.

#### Scenario: A coarser level reads only its own chart
- **WHEN** an atlas whose rectangles hold charts of different objects, and one object's two charts at
  the required gap, is given its mip chain
- **THEN** a bilinear read at every protected level, at the centre of every texel a chart covers,
  SHALL return that chart's own value, wherever the gap falls on the coarse grid
- **AND** a plain 2x2 box chain over the same atlas SHALL NOT

#### Scenario: Every level reaches the device, and a minified surface reads it
- **WHEN** a baked lightmap is uploaded
- **THEN** every level of every plane and of the mask read back from the device SHALL equal the
  bake's values
- **AND** a surface the frame minifies SHALL read the coarser levels, and a magnified one SHALL NOT

### Requirement: A lightmap bake reports its progress and can be cancelled
A lightmap bake SHALL report its progress by stage, counting the texels it has traced, and SHALL stop
at its next step when asked to, reporting that it was cancelled and producing no lightmap. Reporting
progress SHALL NOT change a byte of what a bake that is not cancelled produces.

#### Scenario: A cancelled bake writes nothing
- **WHEN** a bake of a level is cancelled while it traces
- **THEN** it SHALL stop at its next step, report that it was cancelled, and leave the previously
  cooked lightmap unchanged
