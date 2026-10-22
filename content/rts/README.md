# `content/rts/` — the RTS test map

**The Frontier** (`frontier.cymap`): a 128 m square of hills, a lake, a forest, a settlement,
resources and an enemy camp round the 32 m level `samples/13-rts-api`'s Swift game plays on. It is
the map that sample draws with `--render <dir> --map <package>`, and the map M12's slice can grow
from.

![The Frontier from above under blue's fog of war: the level blue has seen, and the rest of the map black, never explored](../../docs/design/images/rts-map-overview.png)

A `.cymap` is a SOURCE: the build cooks it into a `.cypak` (`cy_rts_map_cook`,
`samples/13-rts-api/map/map_cook.h`), and the sample loads the package through the asset system by
asset id (`map_load.h`). Nothing reads the `.cymap` at run time. Licence and provenance are in
[`PROVENANCE.md`](PROVENANCE.md).

## What the cook does with it

| step | engine code | what comes out |
|---|---|---|
| terrain | `cy::terrain::ModifierStack`, `evaluate_region` (the call the editor's terrain tools preview with, #63) | a 129 x 129 lattice, its painted material texels, the stitched mesh |
| bake | the cook's own | one 2048 x 2048 sRGB albedo with a full mip chain: each texel's painted layers, rock on steep ground, sand at the shore, detail from `content/beauty`'s stone and gravel images and value noise |
| foliage | `cy::foliage::generate_region` over a `TerrainQuery` of the cooked tiles | 32 m clusters of the engine's sixteen-byte instances; a road's declared decoration removes what stands on it |
| kit | `map_meshes.cpp` | every building, resource and plant mesh, built from primitives, and the skinned worker on the worker rig's eight joints |
| package | `cy::assets::PackageWriter`, `write_cooked_asset` | ten assets under fixed ids, each in the cooked-asset envelope |

Two cooks of the same source write byte-identical packages; `integration.rts_map` holds that.

## The statements

One statement a line; `#` starts a comment; names in double quotes may hold spaces. Every position
is in WORLD metres, the level's own: the level is the square `base` names, and the map must keep it
flat at height zero, because the game's rules run on it unchanged. A statement the parser does not
know, a missing or malformed number, or a name used before it is declared fails the cook with the
line number.

| statement | meaning |
|---|---|
| `map "<name>"` | the map's name |
| `seed <n>` | the seed of the terrain's noise and of foliage placement (decimal or `0x` hex) |
| `extent <x> <z> <size> <tiles>` | the terrain's square: its corner, its edge in metres, and 64-quad tiles along an edge (1 to 16) |
| `generator <period> <amplitude> <octaves> <base>` | `terrain::TerrainGenerator`: the surface the stack is folded over |
| `water <height>` | the lake's surface |
| `sun <x> <y> <z> <lux>` | the direction the sun's light travels, and its illuminance |
| `base <min_x> <min_z> <max_x> <max_z>` | the level's square |
| `company <x> <z>` | where the lockstep company's field (field metres from its corner) is drawn |
| `texture <name> <path>` | a content image (PNG, relative to the repository root), cooked into the package |
| `layer <i> <name> <r g b> <detail> <metres> <contrast>` | terrain layer `i`, declared in order from 0: its linear colour, and its detail — a texture's name, `noise` or `rows` — repeating every `metres` at `contrast` |
| `slope-layer <i> <from> <full>` | the layer steep ground becomes, from `from` degrees to whole at `full` |
| `shore-layer <i> <metres>` | the layer the ground becomes within `metres` above the water |
| `noise <period> <amplitude> <octaves>` | a `Noise` modifier over the whole map |
| `mound <x> <z> <radius> <height>` | a `Crater` modifier: a hill, or a hollow with a negative height |
| `flatten <min_x> <min_z> <max_x> <max_z> <height>` | a `Flatten` modifier |
| `road <width> <layer> : <x z>...` | a `RoadSpline` modifier, levelled to zero, painting `layer`; the layer is declared to suppress foliage |
| `brush <op> <radius> <strength> <falloff> [arg] : <x z>...` | a `Brush` modifier, the kind the editor's terrain tools record: `raise`, `lower`, `smooth passes <n>`, `flatten height <h>`, `paint layer <i>`. The listed points are a stroke's corners; dabs are spaced a third of a radius along it |
| `material <name> <r g b> <roughness> <metallic> [texture <name>] [emission <r g b>] [transparent]` | the engine's standard material: base colour (linear), roughness, metallic, a base colour texture (`albedo` is the baked terrain), emission in the frame's radiance |
| `species <name> <mesh> <class> <scale_min> <scale_max> <footprint> <tint_a r g b> <tint_b r g b>` | a foliage species: its kit mesh, `canopy`, `understory` or `scatter`, its scale range, and two tints its instances vary between |
| `rule <species> <per hectare> <spacing> <priority> [slope\|altitude\|noise <a b c d>]... [noise-metres <m>]` | a `foliage::PlacementRule`; each test is a trapezoid, weight 1 between `b` and `c` |
| `exclude <min_x> <min_z> <max_x> <max_z> [feather <m>]` | a `foliage::ExclusionZone` |
| `place <mesh> <building\|resource\|prop\|water> <x> <z> [yaw <deg>] [scale <s>] [team <n>] [tag <name>]` | a kit mesh, standing on the cooked terrain (the water at its level); `team` 1 is blue and 2 red, and a part drawn in the `team` material takes it |

The kit's meshes are `barracks`, `keep`, `house`, `well`, `watchtower`, `beacon`, `crate`,
`banner`, `tent`, `fence`, `crops`, `dock`, `gold_mine`, `gold_rock`, `rock_pile`, `log_pile`,
`outcrop`, `boulder`, `tree_pine`, `tree_oak`, `bush` and `water_plane`.
