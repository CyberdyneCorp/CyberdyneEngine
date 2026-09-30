# Lighting authoring and lightmap baking

A walkthrough for a contributor or an agent: how a world's static lighting is authored in the
editor, how the editor turns it into the level's bake description, and how `cy_build lightmap`
bakes the lightmap and the irradiance volumes from it.

**Governed by**: [`rendering-global-illumination`](../../openspec/specs/rendering-global-illumination/spec.md)
("Lightmap baking", light mobility, the shadow mask) and
[`editor-architecture`](../../openspec/specs/editor-architecture/spec.md) (the lighting and
lightmap baking editor). The module READMEs linked below are the reference; this guide is the route
through them.

| Where | What |
|---|---|
| [`src/rendering/lightmap_bake/`](../../src/rendering/lightmap_bake/README.md) | The bake: atlas, path tracer, denoise, seams, mips, progress and cancel, irradiance volume capture, the cooked lightmap and probe payloads |
| [`tools/build/`](../../tools/build/README.md) | The `lightmap` producer and `cy_build lightmap`: the `cylightmap 1` description, the level key, the probe output |
| [`editor/`](../../editor/README.md) ("Lighting & lightmaps") | The lighting editor, its commands and the description writer |
| [`src/rendering/lightmaps/`](../../src/rendering/lightmaps/README.md) | The frame's use of a cooked lightmap and the density view |

## 1. One pipeline

There is one bake and one way into it. The editor never bakes in process: it writes the level's
`.cylightmap` description from the open world and runs `cy_build lightmap` over it as a long
operation. The same description can be a node in the build graph (the `lightmap` producer), so a
shipped level and an editor bake are the same code over the same input.

```text
world document ──(lightmap_description::write)──▶ worlds/x.cylightmap
                                                       │
                   cy_build lightmap --description … --out .cy/cooked/lightmaps/x.lightmap
                                                       │
                          ┌────────────────────────────┴───────────────────────────┐
                x.lightmap (atlas, shadow mask, mips)                x.cyprobes (volumes)
```

## 2. Authoring

Every edit is an undoable transaction through a registered command, so the Lighting panel, the
Inspector rows and an MCP agent all do the same thing:

| Command | What it authors |
|---|---|
| `lighting.light.set-mobility` | `static` (direct light and bounce baked), `stationary` (bounce and a shadow-mask channel baked; the default), `movable` (nothing baked) |
| `lighting.object.set-resolution` | the object's lightmap resolution over the level's density, and `receives=false` for an object that only occludes |
| `lighting.volume.create`, `lighting.volume.set` | an irradiance volume: its transform is probe (0, 0, 0); spacing, probes per axis and capture rays |

## 3. Baking

`lighting.bake-lightmaps` with no `description` writes `worlds/x.cylightmap` beside the world and
bakes it with the form's level settings (encoding, texel density, page size, samples, bounces).
Lights, instances and volumes carry `id <engine identity>`, so the cooked lightmap's shadow-mask
channels and directly baked lights, and each captured volume, name the scene objects they came
from. The description is never edited by hand: the next bake replaces it.

- **Unchanged world, no bake.** The file is rewritten only when its bytes change, and
  `cy_build lightmap` keys the level with the build graph's derivation over the description and
  every file it reads; a matching key reports `cached=1` and writes nothing.
- **Cancel.** The panel's Cancel, the progress surface's, or `lighting.cancel-lightmap-bake` stops
  the tool at its next check. The trace checks at an interval bounded by samples as well as texels,
  so even a 2048-sample bake stops within a fraction of a second, and a cancelled bake leaves the
  previous outputs in place.
- **What it made.** The panel reads the probes back from `x.cyprobes` and draws them seen from
  above, each in its light; `viewport.view-mode.lightmap-density` and `viewport.view-mode.gi-probes`
  request the engine's debug views.

## 4. Testing

| Suite | Holds |
|---|---|
| `integration.render_lightmap_bake` | the progress interval, a 2048-sample bake stopping within a second of its cancel, volume capture (a movable light leaves the probes dark), the probe payload round trip |
| `integration.build_content` | `tools/build/tests/data/editor_level/` — a level exactly as the editor writes it — reads back as the world it came from; mobility and resolution change the bake; the written probes equal a direct capture; the key; old descriptions |
| `integration.build_lightmap_cli` | the command line: progress, cancel, `cached=1` for an unchanged level, the editor's level with its probes, cooked materials |
| `cargo test -p cy-editor-services --test the_bake_is_of_the_authored_world` | the editor writes that fixture byte for byte; authoring changes the description; the real tool bakes what was authored |

The mutation proofs for all of it are in
`openspec/changes/add-editor-lighting-tools/evidence/falsification.txt`.

## 5. Not built yet

No frame draws the GI probe view, and the editor-hosted runtime draws no debug view and loads no
cooked lightmap. Spot cones bake as point lights (`gi::GiLight` has no cone); material graphs bake
with the default grey; the world does not author a sky for the bake yet.
