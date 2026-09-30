# Tasks

## 1. Engine

- [x] 1.1 `lightmap_progress_interval(samples)` on `LightmapBakeProgress`: the trace checks at most 1024 texels and 1024 × 16 samples apart; a 2048-sample bake stops within a second of its cancel.
- [x] 1.2 `capture_irradiance_volumes` over the bake's tracer, reporting the `probes` stage and stopping on the cancel before each volume (a regression case: it read the cancel only after the next volume was captured); a movable light leaves the probes dark.
- [x] 1.3 `lightmap_bake/probes.h`: the probe payload, encoded and decoded, refusing short and foreign payloads.
- [x] 1.4 `cylightmap 1` additions: `id`, `occluder`, `cooked` materials with `tint`, `.cyprim` instances, `volume` lines; old descriptions read as before; bad ids and grids refused by name.
- [x] 1.5 The `lightmap` producer (version 4) writes the probes to an optional second output, and refuses volumes on a node without one.
- [x] 1.6 `lightmap_level_key` and `cy_build lightmap`'s `<out>.cykey`: an unchanged level reports `cached=1` and writes nothing; `--probes`, `<out>.cyprobes`, and a stale probe file removed.
- [x] 1.7 `DebugViewMode::GiProbes`.

## 2. Editor

- [x] 2.1 `cy_editor_services::lighting`: volume, resolution and mobility commands, each one transaction, and the read model.
- [x] 2.2 `cy_editor_services::lightmap_description`: the description from the world (world transforms through parents, the drawn material and tint, disabled lights skipped, spots as points), `write_if_changed`.
- [x] 2.3 `lighting.bake-lightmaps` with no description writes the world's description first; `lighting.write-lightmap-description`; level settings as parameters; the outcome's volumes, probes and `cached`; `read_probes`.
- [x] 2.4 The lighting panel (#68's `LightingTool`): level settings, volumes and their grid, light mobility, the result line, the probe view; the Inspector's Mobility and Lightmap resolution rows.
- [x] 2.5 `viewport.view-mode.gi-probes`.

## 3. Tests and records

- [x] 3.1 C++: `integration.render_lightmap_bake` (interval, cancel latency, volume capture, probe payload) and `integration.build_content` (the editor's fixture read back, mobility, resolution, probes against a direct capture, the key, old descriptions, the producer's probe output).
- [x] 3.2 `integration.build_lightmap_cli`: `cached=1`, the editor's level and its probes, a stale probe file removed, cooked materials and tint.
- [x] 3.3 Rust: the description written byte for byte as `tools/build/tests/data/editor_level/`, authoring changing it, an unchanged world not rewritten, a bake of the world, the real tool baking what was authored; the panel and Inspector through egui frames and AccessKit; MCP authoring with undo and the description written over MCP inside the connection's directory.
- [x] 3.4 Mutation proofs in `evidence/falsification.txt` (`evidence/mutate.py`).
- [x] 3.5 `tools/roadmap/requirements-coverage.toml`, `editor/README.md`, the `lightmap_bake` and `tools/build` READMEs, `docs/guides/lighting.md`.
- [ ] 3.6 A frame drawing `GiProbes`, and the editor-hosted runtime loading a cooked lightmap and its probes. Out of scope: no frame draws any editor debug view yet.
