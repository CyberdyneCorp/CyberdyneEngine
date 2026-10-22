# Design: fog of war

## Two halves, one direction

The grid is simulation and the picture is presentation, and the dependency runs one way:

```
fixed step ─► FogAdapter::update ─► FogOfWar (integer grid, ghosts, digest)
                                         │ write_states(team)  — one byte a cell, read only
                                         ▼
frame      ─► VisibilityField (fade, pack) ─► FogOfWarPass (upload, dispatch) ─► the frame
                                         └──► compose_minimap_fog ─► the HUD's atlas page
```

`cy::rendering-fog-of-war` links no gameplay code. Nothing it does — the fade, the smoothing, the
colours — can reach the grid, so two lockstep peers that draw the fog differently still agree about
every cell, every ghost and every legal order.

## The grid

A position is a Q32.32 metre (`detmath::Fixed::raw`), passed as the raw `i64` so the module needs
no math library. It is located to 1/256 of a cell by integer division (`to_sub_cells`: whole cells
times 256 plus the remainder's share — no 128-bit product), and a cell is in a unit's sight when
`dx² + dz² ≤ r²` in those sub-cells, with the range clamped to the grid before anything is squared.
Positions left of the origin floor rather than truncate. Teams are bits: `visible` and `explored`
are a `u32` each per cell, and a team's view is `mask & share[team]`, so shared vision is one AND.

Ghosts are per observer and per structure, kept sorted by (observer, id). Each update refreshes the
ghost of every structure an observer sees, and forgets every ghost whose PLACE the observer sees
while not seeing its unit there — destroyed or moved. Exploration only grows; a team's view of it
is the union of its allies'.

## The stage

`FramePassKind::FogOfWar` is appended to the enum (after `Present`); its PLACE in the frame is the
declaration order: after the temporal stage — the fade is not smeared by history and the depth is
the one the resolved image was drawn from — and before depth of field, motion blur and bloom, on
linear HDR, because the fog is a property of the surfaces: a lens blurs it, and bloom sees a fogged
light as the dimmer light it is. The frame creates `FrameResources::fog_of_war` (the output extent,
the scene colour's format, storage) and the producer's last pass writes it; the chain continues
from it. Like depth of field, there is no single-pass stand-in, and a multisampled frame needs the
prepass's single-sample depth.

The producer declares two passes: a transfer of this frame's texels from a host-visible ring slot
into the visibility texture (`Rgba8Unorm`, a texel a cell, imported UNDEFINED each frame because
every texel is written), and one compute dispatch. The dispatch reconstructs a pixel's
camera-relative position from the depth through the inverse of `projection × rotation` (passed as
four columns, so no matrix-layout convention is involved), samples the texture with four bilinear
`Load` taps half a cell apart — a tent two cells wide that rounds the staircase of the cells — eases
each level by a smoothstep, and shades. A visible pixel, and the cleared sky, are written as read.

`sample_visibility` and `shade_fog` are the host twins, in the shader's order; `render.fog_of_war`
holds the device to them at every pixel of a frame.

## The ABI

`fog_set_vision` is `[N F]` and refused in a frame update: giving sight is simulation. The
questions are `[N F U]`: a frame reads the grid to draw it. A reveal radius is a `CyFixed` and a
position a `CyFixedVec2`, so nothing crosses as a float. The adapter keeps its visions ordered by
entity, so the units reach the grid in one order whatever order a game gave their visions in.

## Measured

On the RTX 5060 at 480x270 (`smoke.rts_api_fog`), the stage's device time is about 0.011 ms a frame
(worst 0.07 ms), and the host's half — the grid read, the 32 x 32 field faded and packed, the camp
placed — well under a millisecond. A whole frame of `samples/13-rts-api` with the fog and the GI is
4.84 ms against 4.68 ms without the fog.
