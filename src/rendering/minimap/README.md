# `src/rendering/minimap/` — the topographic minimap

`cy::rendering-minimap`, layer 4. A minimap of a height field drawn as a map draws ground, with a
team's fog of war over it and the units and buildings it may see as icons
(`add-rts-combat-and-minimap`, `rendering-post-processing`'s "Topographic minimap"). Host code only —
no device, no shader — so every backend shows the same picture, an RGBA8 image a HUD puts on an
atlas page.

![The Frontier's minimap, baked: its painted ground, hills shaded from the top left with a contour
every metre, the lake by its depth with its shore lined, the roads](../../../docs/design/images/rts-topographic-minimap.png)

## Three stages, each cheaper and more frequent than the last

| Stage | When | What |
|---|---|---|
| `bake_topographic` | once, when the map loads | the heights under each texel (`HeightGrid::at`, bilinear), the ground's colours (a map passes its painted albedo), hillshading — the Lambert term of a light from the image's top left over the flat ground's, so flat ground keeps its colour exactly — contour lines (`classify_contours`), water by its depth below the water level and the shore along it, roads laid over |
| `apply_minimap_fog` | when the team's field changed | each texel through fog_of_war's `sample_visibility` at its world point — the frame's own sampler, so the minimap and the frame never disagree — visible as baked, explored grey and darkened, never explored black |
| `draw_icons` | every composition | dots for units, squares for buildings, outlined, on a copy |

`TopographicMinimap` keeps the three together. Its fog stage compares the field with the last one
it drew and redraws only the texels over changed cells and their neighbours — a sample reads the
cells within one cell of its point — which `unit.render_minimap` holds equal, texel for texel, to the
whole stage. On a battle's map of 256 × 256 texels that is 0.4 to 1 ms every sixth frame, where the
whole stage is 4.5 ms.

## Contours that can be checked

A texel carries a line when a level `k × interval` lies between its height and a neighbour's lower
one; the line sits on the higher texel, so it is one texel wide, and an index line every fifth level.
Ground below the water level carries none; dry ground beside it is the shore. That rule is exact, so
the tests check it at every texel: `unit.render_minimap` over a rolling field and a ramp (lines an
interval apart, the index line), and `integration.rts_map` over the cooked Frontier's own heights —
65 536 texels, 5 558 on a line, 553 on the shore, none misplaced.

## Tests

`unit.render_minimap` (`tests/test_topographic.cpp`): the contour rule exhaustively, the ramp, the
hillshade's lit and shaded faces and flat ground unchanged, water by depth and the shore, roads, the
fog's three states, icons at their world points and off the map not at all, the fog stage reused
while the field stands and redrawn where it moves, the incremental stage equal to the whole one (a
field settled, and one still fading), and what the bake and the fog refuse.
