# Tasks

## 1. An engine consumer of `cy::save` (`m11a:save-has-an-engine-consumer`)

- [x] 1.1 Add `cy::world-persistence` in `src/world/persistence/` with `to_save_overlay()` and `from_save_overlay()`, descriptors read from `ecs::ComponentRegistry`, and a refusal for created entities, positions, blobs, layer states and world variables
- [x] 1.2 Leave a removed entity's world-overlay overrides out of the save, since a tombstone is the whole delta and `save::Overlay` refuses a write to one
- [x] 1.3 Add per-cell dirty tracking to `world::PersistenceOverlay` and a `dirty_cells_only` capture
- [x] 1.4 Move `samples/06-open-world` onto the module and delete its own translation
- [x] 1.5 Add `unit.world_persistence`: a round trip through a committed `SaveArchive` equals the original overlay, and one changed field is detected
- [x] 1.6 Rewrite the criterion to run those cases, declare the mutation that turns it red, and delete its known-gap declaration

## 2. The large-world save benchmark (`m11a:save-benchmark`)

- [x] 2.1 Add `benchmarks/save/` with two bodies — 1 048 576 and 65 536 persistent objects, 20 480 dirty records each — measuring one autosave through the engine's capture and region encoder
- [x] 2.2 Record both entries in `benchmarks/baseline.json`
- [x] 2.3 Rewrite the criterion to run the benchmark, compare against the baseline and check that the two bodies stay within 1.5x, declare the mutation that turns it red, and delete its known-gap declaration

## 3. The save inspector (`m11a:save-inspector`)

- [x] 3.1 Add `cy/save/inspect.h`: `inspect_save()` reads one generation through `SaveArchive::load_generation` (now public) and reports the manifest, retained generations, entity counts and size by scope, region, component and plugin, each size measured by the container's own encoder
- [x] 3.2 Report why a save would not be restored under a caller's policy, and still read its contents under a permissive one; give `LoadReport` an owned `subject` so the build, plugin or type a failure names outlives the load that found it
- [x] 3.3 Add `explain_fields()`: component, field, persistence trait, owning module, entity, and the oldest retained generation from which the field has held its value
- [x] 3.4 Add the semantic diff — `diff_saves()` / `diff_overlays()` — over stable identifiers: entities created, destroyed and reverted, fields added, removed and changed, fragments, and manifest-level differences
- [x] 3.5 Add `cy_save_inspect` (`tools/save/inspect/`) and `just diagnose-save`, printing the three answers in a stable line format
- [x] 3.6 Commit `src/save/tests/data/inspect-campaign/`, a three-generation fixture save written by `inspect_fixture.h` and held byte-identical to it by a case
- [x] 3.7 Add the `a save is inspected*` and `a semantic diff*` cases to `unit.save`, rewrite `m11a:save-inspector` to run them and the tool over the fixture, declare its mutation, and delete its known-gap declaration

## 4. The forbidden save patterns (`m11a:save-forbidden-patterns-checked`)

- [x] 4.1 Add `tools/save/check_forbidden.py`: the ten patterns in the specification's order, a static check over save code for the seven that are shapes of code, and a run of the `forbidden save pattern <id>:` cases of `unit.save` for the nine that are properties of what a save does
- [x] 4.2 Add `tools/save/selftest.py`: every static check planted in a copy of the real tree and reported, prose left alone, and an empty tree refused
- [x] 4.3 Add `src/save/tests/test_forbidden.cpp`, one or more cases per runtime pattern
- [x] 4.4 Fix the defects the runtime cases found — a failed load leaving part of a save in the caller's overlay, and a refused load's `detail` pointing into a manifest the load had already destroyed — each with a regression case watched red first
- [x] 4.5 Rewrite `m11a:save-forbidden-patterns-checked` to run the selftest and the checker with the suite binary, declare its mutation, and delete its known-gap declaration
- [x] 4.6 Update `src/save/README.md`: rows 18 and 20 of the audit, and the reading order
