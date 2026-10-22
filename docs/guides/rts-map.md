# The RTS test map

A map that looks like a game, cooked from a text source by the engine's own terrain and foliage code,
loaded through the asset system and drawn under the Swift RTS of `samples/13-rts-api`. This guide is
the route through it: how to change the map, how it becomes a package, how a host loads and draws
it, and what holds it.

![The Frontier under the RTS sample's session](../design/images/rts-map-game.png)

## 1. The pieces

| Piece | Where | What it does |
|---|---|---|
| The source | `content/rts/frontier.cymap` | the terrain's modifier stack, its layers, materials, foliage rules and placements. [`content/rts/README.md`](../../content/rts/README.md) is the statement reference |
| The cook | `samples/13-rts-api/map/map_cook.h`, `cy_rts_map_cook` | the source to a `.cypak`: `terrain::evaluate_region`, the albedo bake, `foliage::generate_region`, the kit, the package |
| The kit | `samples/13-rts-api/map/map_meshes.h` | buildings, resources, plants and the skinned worker, built from primitives |
| The format | `samples/13-rts-api/map/map_format.h` | the package's ten assets, their ids and their payloads; `content_digest` |
| The loader | `samples/13-rts-api/map/map_load.h` | the package mounted, every asset read by id through `AssetSystem`, kind and hash checked |
| The renderer | `samples/13-rts-api/host/map_render.h` | the session drawn on the map with the engine's frame |

## 2. Change the map

Edit `content/rts/frontier.cymap` and build: the package is a build output
(`build/<profile>/samples/13-rts-api/frontier.cypak`) and is cooked again whenever the source, the
cook or the three content images change.

* **The ground.** `mound`, `flatten`, `road` and `brush` statements are applied in order over the
  `generator`, exactly as the editor's terrain tools apply their modifiers; a later statement sees
  what the earlier ones made. Keep the level's square (`base`) flat at zero: the game's rules run on
  it, and `integration.rts_map` checks it.
* **The paint.** `brush paint ... layer <i>` writes a layer into the texels; `slope-layer` and
  `shore-layer` are applied by the bake on top of whatever was painted.
* **The forest.** `species`, `rule` and `exclude` are the engine's placement rules, unchanged. The
  cook prints what placement considered and why it refused (`candidates=`, `rejected_weight=`,
  `rejected_spacing=`, `rejected_exclusion=`, `on_roads=`): spacing is resolved order-free across
  every rule, so a dense rule beside a wide one thins both.
* **The buildings.** `place <mesh> <kind> <x> <z>` stands a kit mesh on the cooked ground. A new mesh
  is a function in `map_meshes.cpp` and a name in `kLibraryMeshes`.

Run `cy_rts_map_cook --source content/rts/frontier.cymap --content-root . --out /tmp/f.cypak`
to see the cook's line without a build.

## 3. Draw it

```
cy_sample_rts-api --render <dir> --map <frontier.cypak>                     the player's view, 1080p
cy_sample_rts-api --render <dir> --map <frontier.cypak> --view overview \
    --width 1280 --height 720 --video <frames> --video-every 2              the video's frames
just capture-rts-map                                                        every image and the video
```

The controls switch one feature off each: `--gi off`, `--no-fog`, `--no-hud`, `--no-ao`,
`--no-shadows`; `--no-validation` drops the graphics API's validation layer for a measurement.

A host of its own needs five things: the package loaded first (`load_map`), the map's buildings
handed to the GI as boxes before the GI is built (`map_gi_boxes`, `HostOptions::extra_gi_boxes`),
the fog of war's grid sized over the map's ground (`map::map_bounds`, `HostOptions::fog_bounds`,
`host/fog_grid.h`), a `MapRenderer` started over the host, and `frame()` after every host frame.
`MapRenderer::start` refuses a host whose fog grid does not cover the map. The map does not change
the game's rules: what it changes is which ground the GI holds and the fog's grid covers.
`cy_sample_rts-api --map <frontier.cypak>` without `--render` runs the session headless on it.

## 4. What holds it

| Suite | What it decides |
|---|---|
| `integration.rts_map` | two cooks write byte-identical packages; the package loads back by id to the content cooked, digest for digest; the level flat, hills and a lake, painted layers and roads, buildings on the ground, a forest kept off the level and the roads, the skinned worker; damaged, mistyped, newer and truncated assets and malformed sources refused with their reasons |
| `integration.rts_map` (fog) | the fog grid a run on the map builds covers the cooked terrain and the level; a unit of another side far from blue's sight, or off the grid, is not drawn |
| `smoke.rts_api_map` | the session on the map: its gameplay report the headless run's on the same map (the lockstep digest included) and every fog decision the run's without a map; the fog grid the map's 128 m and the company's second group withheld; 420 frames drawn with skinned draws and no validation error, frame 420 against `tests/references/rts_map_420.png`, and a 1080p frame within 16.7 ms |
| `integration.render_assembly` | 'the sky's irradiance is reused while the atmosphere, the eye and the sun hold': the assembly cost the map's measurement found |

Each was proved red by a mutation: a loader that skips the hash, a cook that leaves plants on the
roads, a renderer that drops the skinned draws, an assembly that integrates the sky every frame.

## 5. Pitfalls

* **The forest is merged.** Plants are baked into one mesh a 32 m square, species and tint bucket;
  a renderer that wants wind, promotion or per-instance culling reads `FoliageData::plants` instead.
* **The water is opaque.** The standard material has no opacity term, so the lake is a dark, glossy
  surface rather than a transparent one.
* **The fog's grid must cover everything drawn.** Ground off every grid is never explored — the
  stage draws it black and the simulation answers `Unexplored` — so a grid smaller than the map
  blacks out the ground past it whatever blue has seen. Size the grid from the cooked map
  (`map::map_bounds`); `MapRenderer::start` refuses one that does not cover it. Only blue's own
  units are drawn regardless of the fog: the camp and the company's second group are drawn only
  where blue sees them.
* **Metal and D3D12** compile the frame's shaders and were not run on this map; the measurements are
  the RTX 5060's, on Vulkan.
