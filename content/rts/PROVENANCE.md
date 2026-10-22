# `content/rts/` — licence and provenance

## What is here

| file | what it is |
|---|---|
| `frontier.cymap` | the RTS test map's source: the terrain's modifier stack, its layers, its materials, its foliage rules and its placements, written by hand in this repository |
| `README.md` | the `.cymap` statement reference |

## Licence

**The same licence as the rest of this repository** — see [`LICENSE`](../../LICENSE). Nothing here is
a third-party asset, nothing was downloaded, and nothing is derived from anything outside this tree.
`THIRD_PARTY.md` therefore lists none of it.

## What the cooked map is made of

Everything the cooked package holds is a pure function of `frontier.cymap`, the code that cooks it
and three images already in this tree:

- **The terrain, its painting and the forest** are computed by the engine (`cy::terrain`,
  `cy::foliage`) from the source's seed and statements.
- **Every mesh** — buildings, resources, plants, the skinned worker — is built from primitives by
  `samples/13-rts-api/map/map_meshes.cpp`. No mesh is a file.
- **The textures** are `content/beauty/textures/stone_albedo.png`, `gravel_albedo.png` and
  `copper_albedo.png`, the project's own generated images
  ([their record](../beauty/textures/PROVENANCE.md)), and the terrain's albedo, which the cook bakes.

## How to check it

```
just build-engine                       # cooks build/<profile>/samples/13-rts-api/frontier.cypak
ctest -R integration.rts_map            # two cooks byte-identical, and the package loads back
```
