# Design: selection outlines and unit highlights

## Where the stage sits

After the post-process, before the interface. Before bloom the outline would bloom, which
`editor-visual-language` forbids ("SHALL NOT bloom or glow"); before exposure its colour would be
the scene's exposure applied to a colour a game chose; before the tone curve it would be a radiance
the curve reshapes. After the curve the colour a game asks for is the output's bytes, which is what
`render.selection_outlines` checks exactly. The hovered glow is a falloff the edge pass draws, not a
use of the bloom chain, for the same reason. The interface is drawn afterwards because it sits over
the world and its outlines alike.

## A mask drawn again, not an id channel in the prepass

The frame's depth prepass could have written an object id into another attachment. That would add
an output to `cy/frame.slang`'s prepass for every frame, outlined or not, and change a shader other
work depends on. Drawing only the marked draws again costs nothing when nothing is marked, keeps the
frame shaders untouched, and — because the mask pass tests marked objects only against each other —
gives the WHOLE silhouette of a partly hidden unit, which is exactly what the occluded style needs.
The mask pass goes through the frame's own position stream, draw records, instance rows and sets 0
and 1 (`selection_mask.slang` imports `cy.frame`), so a visible marked surface lands on the prepass
depth to the last few bits; `depth_tolerance` absorbs those bits.

## One push word

The frame's pipeline layout carries one 32-bit push constant, and a layout with a different push
range is not compatible with the frame's sets. So the mask's push word is the draw index in 24 bits
and the style slot in 8. That caps styles at 255 distinct (kind, colour) pairs in use at once, which
the palette enforces and compacts toward; it does not cap units.

## The edge pass

Per pixel outside every silhouette: the nearest marked pixel within the widest style's reach,
scanned row by row and replaced only by a strictly nearer one, so a tie goes to the first in scan
order on the device and on the host alike. Its style decides the width it reaches, the solid or
falling-off alpha, and, when that marked pixel is hidden, the occluded alpha and the diagonal dash.
Inside a silhouette: nothing unless the surface is hidden, then the fill. A pixel with nothing to
draw is discarded, so an untouched pixel is untouched by construction rather than by the blend
unit's rounding — which is what makes "nothing marked" byte-identical, and the composite is not
recorded at all when no marked draw was drawn.

The search is a brute-force disc: at the eight-pixel maximum, 289 taps a pixel. A separable exact
distance transform (a row pass, then a column pass) would bring that to 34 at the price of a second
target; it is the change to make if widths grow.

## Gameplay: a component by name

A mark is gameplay state the renderer shows, so it is a component on the unit's entity. It is
registered by name (`ComponentRegistry::register_builtin`) rather than reflected: it is presentation
— never authored, saved or replicated — and the name route is what `world_find_component`,
`world_add_component` and `world_borrow_component` reach from Swift with no new ABI entry. The
draws' stable identity is the entity's bits (`extract.cpp`), so `gather_highlights` keys marks by
exactly what the frame's draws carry.

## Tests

`render.selection_outlines` renders `pipeline_test::FrameScene` arranged as a small field. (e) pins
the frame with the stage absent to a committed reference drawn before the stage existed, and holds
the stage with nothing marked byte-identical to it. (a) checks the mask against the boxes' own
projections, every changed pixel against the width, and every pixel against the host reference
blended over the plain frame. (b), (c) and (d) are the requirement's scenarios measured on the
geometry: the far unit untouched, the ring's colour exact and its width exact, and the hidden part
of the unit behind the building found under the building and nowhere else.
