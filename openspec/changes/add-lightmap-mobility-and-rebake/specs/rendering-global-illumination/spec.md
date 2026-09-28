## ADDED Requirements

### Requirement: A light's mobility decides what of it the lightmap bakes
Every GI light SHALL declare a mobility, `Static`, `Stationary` or `Movable`, with `Stationary` as
the default. The lightmap bake SHALL bake a `Static` light's direct term at the receiver together
with its indirect light; SHALL bake a `Stationary` light's indirect light and a shadow-mask channel
holding the fraction of the light's extent visible from each texel, and SHALL NOT bake its direct
term; and SHALL bake nothing of a `Movable` light, not even its bounce. A bake whose lights declare
no mobility SHALL produce the same texel planes it produced before mobility existed.

The shadow mask SHALL be one RGBA plane in the atlas layout, with one channel per stationary light
in the scene's order and the channel's light identity recorded beside it. A channel with no light
SHALL be fully lit. A level with more than four stationary lights SHALL be refused by name. The mask
SHALL be dilated within each chart like the texels, and the cooked lightmap SHALL carry it.

#### Scenario: A stationary light bakes its indirect and a shadow mask, not its direct
- **WHEN** a room is baked with one `Stationary` light and a wall shadowing half the floor
- **THEN** the texels SHALL hold the light's bounce but not its direct term at the receiver
- **AND** the shadow mask SHALL read near zero on the shadowed floor and near one on the lit floor
- **AND** the light's direct term evaluated at a floor point, scaled by the mask sampled there,
  SHALL match the path tracer's shadowed direct term at that point, at any intensity the light is
  given at runtime

#### Scenario: A movable light bakes nothing
- **WHEN** the same room is baked with its light `Movable`
- **THEN** the texels SHALL hold only the sky's and the emissive surfaces' light, and no shadow-mask
  channel SHALL be assigned

#### Scenario: A static light bakes its direct term
- **WHEN** the same room is baked with its light `Static`
- **THEN** the texels SHALL match those of the pre-mobility `DirectAndIndirect` bake of that room

### Requirement: Moving an object re-solves its region and keeps the rest byte for byte
The bake SHALL rebake a level incrementally given the previous bake, the moved instances and their
previous world bounds. It SHALL re-solve every object that moved, and every object with a surface
texel within an influence distance of a moved object's previous or new bounds. Every texel of every
other object SHALL be copied from the previous bake unchanged, byte for byte, including coverage and
the shadow mask. When the level no longer packs to the same rectangles, or the bake's mode or page
layout changed, the rebake SHALL fall back to a full bake and report that it did.

#### Scenario: One moved object in a larger level
- **WHEN** one object of a level of several separated objects is moved and the level is rebaked
  incrementally
- **THEN** only the objects inside the influence distance SHALL be traced
- **AND** every texel of every other object SHALL be byte-identical to the previous bake
- **AND** the re-solved objects SHALL agree with a full bake of the moved level within the bake's
  stated noise bound

#### Scenario: Nothing moved
- **WHEN** a level is rebaked incrementally with no moved instances
- **THEN** no texel SHALL be traced and the result SHALL be byte-identical to the previous bake

### Requirement: The bake checks that chart padding survives the atlas rectangle
The bake SHALL measure, on its own rasterisation, the gap in empty texels between every two distinct
charts of one object, and SHALL report every object whose gap is smaller than two texels of the
coarsest mip level the atlas protects (two texels at the base level alone). When the bake settings ask it to, the bake SHALL refuse such a level, naming the instance.

#### Scenario: A rectangle at a lower density than the unwrap shrinks its padding
- **WHEN** a mesh unwrapped with a padding sized for its own density is baked at a lower texel
  density, so its charts land closer than the required gap
- **THEN** the bake SHALL report that object's padding as short
- **AND** the same mesh at a density that keeps the gap SHALL NOT be reported
