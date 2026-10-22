# Fix the fog of war on the RTS test map: the grid covers the map

## Why

On the RTS test map (#133, "The Frontier") blue's fog of war (#132) was drawn only inside a square
round the level: a dark band surrounded a lit patch near blue's units, and everything past the
square — trees, buildings, terrain, and the company's units on their field north of the level — was
drawn as if seen. `rts-map-game.png` and `rts-map-overview.png` showed it, against #133's claim that
the fog covered the whole map.

The cause was three decisions, each of which hid the others:

- the host's fog grid (`samples/13-rts-api/host/fog.h`) was the level's 32 x 32 metres, sized for
  the level before the map existed, while the frame drew 128 m of terrain;
- the map's renderer padded the 128 m field it uploaded with EXPLORED cells round the level's grid,
  under a style lightened "so the map still reads as a place", and drew the company regardless of
  the fog — so ground no blue unit had ever been near was drawn nearly as lit as ground it saw;
- and the stage itself clamped a sample off the grid to the nearest edge cell, so any grid smaller
  than the frame's ground carried its edge's state out over everything past it.

## What changes

- A point off the grid is NEVER EXPLORED: `fog_of_war.slang` and its host twin `sample_visibility`
  return zero for both levels outside the grid's rectangle, so the frame and the minimap
  (`compose_minimap_fog`) shade it the unexplored colour; inside, sampling is unchanged.
- The sample's fog grid is sized from what the run draws (`host/fog_grid.h`): the level's square
  without a map, and the cooked map's bounds (`map::map_bounds`: the terrain's square and the
  level's) with `--map`, through `HostOptions::fog_bounds`. Cells stay a metre, edges on whole
  metres, so the level's cells are the cells they were.
- The map renderer uploads the host's grid as it is — no padding, the engine's default style — and
  the minimap's overlay is composed from that same field over the level's square, so the two agree.
  `MapRenderer::start` refuses a host whose grid does not cover the map. The company's second group
  is drawn only where blue sees it (`shown_by_fog`), as the camp's raiders already were.
- `cy_sample_rts-api --map <package>` runs headless on the map, so `smoke.rts_api_map` holds the
  drawn run to a headless run on the same grid, and every fog decision to the run without a map.

## Impact

- `rendering-post-processing`, Fog of war rendering: a surface off the grid is never explored.
- `gameplay-framework`, A strategy test map: the fog's grid covers the whole map.
- The map run's `rts fog` line reports the 128 m grid, its digest, and the cells blue sees past the
  level's edge, which the level's grid cut off; every other fog decision is the level run's. `rts_map_420.png` and the map's images and video are recaptured.
