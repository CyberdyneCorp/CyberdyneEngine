# Proposal

## Why

M11.c's gate-findings phase declared four `save-and-persistence` gaps in
`tools/roadmap/milestones/m11a.toml`, all closing at M11.e: `save-inspector`,
`save-forbidden-patterns-checked`, `save-benchmark` and `save-has-an-engine-consumer`. Two of them
are about integration rather than about src/save/ itself:

- **No engine module above src/save/ linked `cy::save`.** The only translation between
  `world::PersistenceOverlay` and `save::Overlay` lived in `samples/06-open-world`, and it could
  describe one component type because it named that type's descriptor. `src/save/README.md` records
  requirement 1 ("A save is the overlay, not a second model") as unexercised in engine code, and its
  criterion counted a string in a `CMakeLists.txt`.
- **The large-world save benchmark the requirement says the engine SHALL maintain did not exist.**
  `benchmarks/` had no save entry, and its criterion checked that the word `save` appeared in
  `benchmarks/baseline.json`.

## What Changes

- Add `cy::world-persistence` (`src/world/persistence/`, layer `scene`): the one translation between
  the world's overlay and a save's, reading every descriptor from the ECS component registry. It
  refuses the world state the save model cannot yet carry losslessly rather than dropping it.
- Give `world::PersistenceOverlay` per-cell dirty tracking, so an autosave captures the cells that
  changed and not the whole overlay.
- `samples/06-open-world` calls the engine module and keeps no translation of its own.
- Add `benchmarks/save/`: one autosave over a world of 1 048 576 persistent objects and over one
  sixteen times smaller, with the same 20 480 dirty records, with committed baselines.
- Replace the two criteria's searches with observations: `save-has-an-engine-consumer` runs the
  round-trip cases of `unit.world_persistence`; `save-benchmark` runs the benchmark, compares it
  against the baseline and requires the two bodies to stay within 1.5x of each other. Each declares
  an engine mutation that turns it red, and both known-gap declarations are deleted.
- Add the save inspector, `cy/save/inspect.h`, and its command line `cy_save_inspect` /
  `just diagnose-save`: what is in a save (manifest, generations, entity counts, size by scope,
  region, component and plugin), why each field is there (component, field, trait, module, entity,
  and the generation it has held its value since), why a save would not be restored, and the
  semantic diff of two generations. `LoadReport` gains an owned `subject`, and
  `SaveArchive::load_generation` becomes public so the inspector reads a save through the loader.
- Add `tools/save/check_forbidden.py` and its selftest: the ten forbidden save patterns, each a
  static check over save code, a runtime case in `unit.save`, or both. Fix the two defects the
  runtime cases found: a failed load left part of a save in the caller's overlay, and a refused
  load's `detail` pointed into a manifest the load had destroyed.
- Replace the `save-inspector` and `save-forbidden-patterns-checked` criteria's absent subjects with
  runs of the cases, the tool and the checker, each with a declared mutation, and delete both
  known-gap declarations.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `save-and-persistence`: add the requirement that the world overlay reaches a save through one
  engine translation, captured cell by cell from what changed.
- `save-and-persistence`: add that the inspector reads through the loader and dates a field by the
  generations retained, and that the forbidden patterns are checked by a checker that can fail.

## Impact

- New target `cy::world-persistence`, new suite `unit.world_persistence`, new runner
  `cy_bench_save` and two entries in `benchmarks/baseline.json`.
- `world::PersistenceOverlay` gains `dirty_cells()`, `dirty_cell_count()`, `clear_dirty()`,
  `override_bytes()` and `variable_count()`; nothing existing changes behaviour.
