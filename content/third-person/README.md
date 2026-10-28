# `content/third-person/` — the third-person demo's level

**The Hillside** (`hillside.cymap`): a walled courtyard of flagstones in a valley of meadow and
woods, a hill rising east of it and a pond in the south-west — the level `samples/14-third-person`'s
Swift hero runs, climbs and jumps through.

![The hero's run: the stairs, a jump, the camera pulled in by the north wall, the ramp, a jump onto the hill, and the view back down](../../docs/design/images/third-person-run.png)

It is written in the RTS test map's format and cooked by the RTS test map's cook
(`cy_rts_map_cook`, `samples/13-rts-api/map/map_cook.h`): the format's statements are listed in
[`content/rts/README.md`](../rts/README.md). The build cooks it into
`build/<profile>/samples/14-third-person/hillside.cypak`, which the sample loads by asset id through
the asset system. Nothing reads the `.cymap` at run time. Licence and provenance are in
[`PROVENANCE.md`](PROVENANCE.md).

## What it holds, and what it does not

| | |
|---|---|
| the terrain | 128 m from (-64, -64), a sample a metre: the engine's generator and modifier stack — noise, the hill east of the courtyard (`mound 36 -4 24 7.5`, a flank of about 20 degrees the hero runs up), ridges closing the valley, the carved pond, the courtyard flattened at height zero with its edges smoothed, two paths |
| the painting | grass, dirt, rock on steep ground, sand at the shore, meadow and moss, and the courtyard's FLAGSTONES — layer 4, the `stone` image's detail at a two-metre repeat — painted in broad strokes over it |
| the materials | the RTS kit's, the courtyard pieces' `flagstone`, `capstone` and `stone_wall`, and the hero's six (`hero_skin`, `hero_hair`, `hero_shirt`, `hero_trousers`, `hero_boots`, `hero_leather`), which `samples/14-third-person/host/hero_mesh.h` names |
| the foliage | pines on the ridges, oaks and bushes in the valley, boulders on the slopes — excluded from the courtyard, its approaches and the hero's line up the hill |
| the placements | three houses, a well, crates, a log pile, banners, a tent and fences round the courtyard; boulders, an outcrop and a rock pile on the hill; a dock on the pond and a watchtower on the knoll |

**The courtyard's stairs, platform, ramp, walls and arch are not here.** The map's kit has no mesh
for them, and they are what the demo is about, so they are the sample's own level pieces — boxes in
`samples/14-third-person/host/level.cpp`, collided and drawn beside what the map places.

## Changing it

Edit `hillside.cymap` and build: the cook runs again because the package depends on the source.
`ctest -R third_person` then says whether the run still climbs the stairs, reaches the hill and
keeps its frame budget — and the golden still (`smoke.third_person_render`) will name the texels a
change moved; rewrite it with `CY_RENDER_UPDATE_GOLDEN=1` once the new picture is the one wanted.
The hero's path crosses the courtyard at x = 0 and runs east along z = -4.5: a placement there is an
obstacle the scripted player does not steer round.
