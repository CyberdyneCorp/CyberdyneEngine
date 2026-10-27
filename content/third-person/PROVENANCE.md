# `content/third-person/` — licence and provenance

## What is here

| file | what it is |
|---|---|
| `hillside.cymap` | the third-person demo's level source: the terrain's modifier stack, its layers, its materials, its foliage rules and its placements, written by hand in this repository |
| `README.md` | what the level holds and how to change it |

## Licence

**The same licence as the rest of this repository** — see [`LICENSE`](../../LICENSE). Nothing here is
a third-party asset, nothing was downloaded, and nothing is derived from anything outside this tree.
`THIRD_PARTY.md` therefore lists none of it.

## What the cooked level is made of

Everything the cooked package holds is a pure function of `hillside.cymap`, the code that cooks it
and three images already in this tree:

- **The terrain, its painting and the forest** are computed by the engine (`cy::terrain`,
  `cy::foliage`) from the source's seed and statements.
- **Every mesh** of the kit is built from primitives by `samples/13-rts-api/map/map_meshes.cpp`. The
  hero is built from primitives by `samples/14-third-person/host/hero_mesh.cpp` at run time. No mesh
  is a file.
- **The textures** are `content/beauty/textures/stone_albedo.png`, `gravel_albedo.png` and
  `copper_albedo.png`, the project's own generated images
  ([their record](../beauty/textures/PROVENANCE.md)), and the terrain's albedo, which the cook bakes.
- **The sounds** the sample plays are synthesised by `samples/14-third-person/host/sounds.cpp` from a
  fixed sequence; no sound is a file.

## How to check it

```
just build-engine                          # cooks build/<profile>/samples/14-third-person/hillside.cypak
ctest -R third_person                      # the run on it, headless and drawn
```
