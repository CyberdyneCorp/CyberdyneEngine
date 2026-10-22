# Tasks

- [x] Treat a point off the grid as never explored in `fog_of_war.slang` and `sample_visibility`; regenerate the committed SPIR-V and MSL.
- [x] Size the sample's fog grid from what the run draws (`host/fog_grid.h`, `map::map_bounds`, `HostOptions::fog_bounds`); let `--map` run headless.
- [x] Upload the host's grid as it is in the map renderer, compose the minimap from the same field, refuse a grid that does not cover the map, and draw the company's second group by blue's sight.
- [x] Regression cases, each red on the old code: `unit.render_fog_of_war` (a point off the grid), `render.fog_of_war` (h) (ground off the grid black), `integration.rts_map` (the grid covers the map; an enemy far from sight not drawn), `smoke.rts_api_map` (the grid's extent, the enemies withheld, the fog decisions against the run without a map).
- [x] Recapture `rts_map_420.png`, the map's images and its video; correct the READMEs, the guides, the requirements map and the living specifications, and validate this change.
