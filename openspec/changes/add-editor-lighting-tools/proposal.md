# Proposal: Lighting authoring tools on the lightmap bake pipeline

## Why

Issue #29 lists "Lighting and probes" among the missing editor tools. #68 (`add-lightmap-frame-shadow-mask`)
gave the editor its lightmap bake: one panel (`editor-lighting-and-lightmap-baking`), one command
(`lighting.bake-lightmaps`, running `cy_build lightmap` over a `.cylightmap` description) and the
density view. But the description is written by hand, so nothing an author does in the editor
reaches the bake: no command sets a light's mobility or an object's lightmap resolution, no
irradiance volume can be placed, and nothing links a description's `light` or `instance` lines to
the scene objects they came from. A 2048-sample bake also takes 6.7 seconds to see a cancel,
because the trace checks every 1024 texels whatever the sample count.

The owner chose one pipeline: #68's design stays, and the EDITOR WRITES the level's description
from the authored world, so the command bakes what was authored.

## What Changes

- **Editor authoring.** Undoable commands, each one transaction and an MCP tool:
  `lighting.volume.create` and `lighting.volume.set` (an `IrradianceVolume`: origin = its
  transform, spacing, probes per axis, rays), `lighting.light.set-mobility` (`LightBakeMobility`:
  static, stationary, movable) and `lighting.object.set-resolution` (`LightmapObject`: resolution
  scale, receives). The components are authoring-only. The lighting panel (#68's `LightingTool`)
  gains the volumes with their grid, every light's mobility, the level settings and the probe view;
  the Inspector gains Mobility and Lightmap resolution rows.
- **The description is written from the world.** `lighting.bake-lightmaps` with no description
  writes `worlds/x.cylightmap` beside the world on every request, then bakes it:
  `cy_editor_services::lightmap_description`. Lights carry their mobility word and engine
  identity; instances their cooked mesh (or `.cyprim` source), world transform, resolution scale,
  identity and `occluder`; materials the cooked material the object draws with, times its tint; and
  volumes their identity and grid. `lighting.write-lightmap-description` does only that step. The
  file is rewritten only when its bytes change.
- **`cylightmap 1`, extended compatibly.** Optional `id <n>` on `light` and `instance`, `occluder`
  on `instance`, `material "<name>" cooked "<path>" [tint <r g b>]`, `.cyprim` instance sources
  (generated and unwrapped as the importer would), and `volume <id> <origin> <spacing> <counts>
  <rays>` lines. A description written before any of them reads exactly as it did.
- **Volumes are baked.** `lightmap_bake::capture_irradiance_volumes` captures each volume with the
  bake's own tracer after the atlas; `lightmap_bake/probes.h` is the payload, written next to the
  atlas (`<out>.cyprobes`) by `cy_build lightmap` and as an optional second output by the `lightmap`
  producer (version 4).
- **An unchanged world is not baked again.** `cy_build lightmap` keys the level with the build
  graph's own `derivation_key` over the description and every file it reads, keeps the key beside
  the output, and answers a matching run with `cached=1` and nothing written.
- **Cancel latency.** The trace checks at `lightmap_progress_interval(samples)` texels — at most
  1024 texels and 1024 × 16 samples — so a 2048-sample bake stops within a second.
- **View mode.** `DebugViewMode::GiProbes` and `viewport.view-mode.gi-probes`, for the probes the
  bake captures; no frame draws it yet.

Dropped from the first version of this change, because #68 already provides them: its own panel id,
`LightmapBakeObserver`, `measure_texel_density` and the panel's atlas view, `lighting.bake.*` and the
in-process `LightingRuntime` bake.

## Capabilities

### Modified Capabilities

- `rendering-global-illumination`: a description names the scene's objects and irradiance volumes,
  which the bake captures; a many-sample bake sees a cancel within bounded work.
- `editor-architecture`: the lighting editor bakes the world as it was authored.
- `editor-viewport-and-gizmos`: GI probes are a selectable engine view mode.

## Impact

Engine: `src/rendering/lightmap_bake` (progress interval, volume capture, probe payload),
`tools/build` (the description, the producer's probe output, the level key, the CLI),
`src/servers/render` (one enum value). Editor: `cy-editor-services` (`lighting`,
`lightmap_description`, `lightmaps`), `cy-editor-commands` (the description write hook),
`cy-editor-interface` (the form's settings), `cy-editor-shell` (the panel and the Inspector rows),
`cy-editor-viewport` (the view mode). A hand-written description bakes byte for byte as before; the
producer version moves to 4, so the graph re-bakes every level once.
