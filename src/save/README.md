# `src/save/` — layer 2

Durable game state: what is saved, how it is encoded, and how it survives being interrupted.

**What belongs here**: the persistence overlay and the persistent state store organised by region;
persistence scopes and the traits that route into them; persistent identity; dirty tracking; the
save container — manifest, content-addressed chunks, structured load failures; migration on the way
in; the journal and its compaction; atomic generations and their retention; the storage backend
interface with its filesystem and in-memory implementations; and the service that captures on the
main thread and writes in the background.

**What does not belong here**: a second serialisation path, any knowledge of the ECS or of a node,
and the partitioner. See `CMakeLists.txt` for each exclusion and its reason.

**Governed by**: `save-and-persistence`. Reached Working at M6.

## The three sentences the module is built around

**The save IS the overlay.** `save-and-persistence` opens with `authored content + persistence
overlay + scoped fragments = saved game` and forbids a second persistence model, because two models
are two sources of truth about whether a bridge is destroyed. `world-partition-and-streaming`
defines the same structure from the other side and hands the encoding, journalling, atomicity,
incrementality and migration to this capability. `Overlay` in `overlay.h` is that one structure: the
world holds it, a save encodes it, and a dedicated server, a replay and the editor's play mode share
it.

**The store is keyed by region, so saving loads nothing.** "Producing a save SHALL NOT require
loading regions that are not resident" is not a promise the save path makes; it is a consequence of
the store holding value records keyed by `RegionKey` and `PersistentId` rather than live entities.
Unloading a region takes nothing away from it. `Overlay::set_residency` exists for the inspector and
is read by nothing that writes a save — and a test encodes the same logical state with different
regions resident and requires the bytes to be equal.

**There is exactly one instant at which the active save changes.** A commit writes chunks under the
hash of their own bytes, verifies them, writes a manifest, and then rewrites one small pointer
object atomically. Everything before that instant is additive and unreferenced, so a process killed
at any point leaves either the previous generation whole or the new one whole. `archive.h` carries
the table of what a kill at each phase leaves behind, and `tests/test_kill_nine.cpp` walks it with a
real `SIGKILL` against a real directory.

## Reading order

| File | What it settles |
|---|---|
| `identity.h` | `PersistentId`, `RegionKey`, `Scope` — and why they are three types rather than three integers. |
| `traits.h` | What a save captures, derived from the declaration that already exists rather than declared again. |
| `overlay.h` | The delta model: modified, created, tombstoned; dirty tracking; scope fragments. |
| `container.h` | The manifest, the chunk encoding, migration on the way in, and the structured load failures. |
| `storage.h` | The backend interface, and the one promise every backend makes: `write` is atomic per key. |
| `archive.h` | Generations, the five-phase commit, the journal, compaction and retention. |
| `service.h` | The bounded capture on the calling thread, and the write on the async thread. |
| `checkpoint.h` | In-memory checkpoints, the memory budget, and the epoch a restore advances. |
| `inspect.h` | The save inspector and the semantic diff: what is in a save, why each field is there, why it would not be restored, and what changed between two — read through the loader, never beside it. |
| `conflict.h` | Which of a local and a remote copy of one save to keep, decided on logical metadata — and a type with no timestamp in it, so it cannot be decided on anything else. |

## Checkpoints are not `SaveKind::Checkpoint` (M9)

`save-and-persistence` asks for two different things with one word. `SaveKind::Checkpoint` in
`service.h` is a **full generation written to storage** — the right answer to "the player quit and
came back". `checkpoint.h` is the other one: *"a save optimised for rapid in-session restoration,
retaining what is required to restore session, world, and participant state without restarting the
application"* — the right answer to "the player died". That path must not touch the filesystem while
the state is still in memory, and it must leave the session running, so a checkpoint here is a
retained overlay plus the moment it was taken at, and a restore is a merge.

**The epoch increment is the load-bearing part.** *"Checkpoint restore SHALL increment the simulation
epoch, so temporal caches and histories treat themselves as stale."* `CheckpointStore::restore()`
takes an `EpochCounter&` and there is no overload that does not, because a restore that left the
epoch alone would move the tick backwards inside one timeline and every stamped cache, handle,
history and log would believe itself current. `tests/test_checkpoint.cpp` stamps a cache before the
restore and requires `determinism::is_stale()` to say so afterwards — and deleting the `advance()`
line is the only mutation that check notices, which is the point of it.

**The budget is a refusal.** *"Checkpoints MAY be retained in memory as well as on storage, subject
to the memory budget."* A store over its slots or its bytes evicts the oldest; an evicted checkpoint
keeps its history entry and loses its state, so `restore()` answers `Unavailable` for one that was
evicted and `NotFound` for one that never existed — two different answers because a caller that
retries is right in one case and wrong in the other. One checkpoint larger than the whole budget is
refused at TAKE time rather than retained over budget or dropped silently.

Bytes are measured the way the container would encode them (`measure_overlay_bytes()`), not
estimated: a budget compared against a guess is not a budget.

## What is not finished, and is written down rather than implied

* **The capture copies the overlay, not the world.** The specification asks for the bounded
  main-thread cost to come from versioning or copy-on-write of the chunks that hold persistent
  state. Copying the delta is bounded by what changed rather than by world size, which is the
  property the exit criterion needs; copy-on-write is a change to `service.cpp` alone.
* **Dirty flags are not cleared by the service.** Clearing exactly the records that were captured
  needs that same per-chunk versioning. Until then `take_outcome()` reports what committed and the
  caller decides, because clearing everything on completion would silently drop changes made during
  the write.
* **An `Overlay` entry costs about 3 KB resident.** Measured by `benchmarks/save/` at M11.e: a
  fixture with every one of 1 048 576 objects holding one two-field record reached 3.2 GB for the
  pair of worlds, so the benchmark carries saved state for one object in sixteen instead. Each entry
  owns its own component array and each component its own value record, and each of those is a
  separate allocation; a store for a world where most objects have changed needs pooled storage.
* **Plugin ownership is declared per save, not per record.** The manifest carries the plugin
  inventory and a load names the missing one; attributing an individual record to its owning module
  needs the module registry that arrives with `project-and-plugins`.
* **Checkpoints are memory-only.** `CheckpointStore` retains and restores; writing a checkpoint
  through to the archive as well is `SaveService`'s existing path and the two are not yet joined, so
  "retained in memory **as well as** on storage" is half-built. A restore therefore cannot fall back
  to storage when the memory copy has been evicted — it reports `Unavailable`, which is honest and
  is not the whole requirement.
* **Encryption is absent, deliberately.** `save-and-persistence` requires that encryption never be
  presented as integrity and that no bespoke cryptography be written. Chunks carry BLAKE3 content
  hashes and the manifest carries theirs; confidentiality is an authenticated-encryption pass over
  the same objects, and it lands with a key management story rather than before one. It also lands
  with a **dependency**: `thirdparty-dependencies` already names **mbedTLS** (Apache 2.0) as this
  engine's cryptography library beside BLAKE3, and adopting one "SHALL go through the OpenSpec
  change flow recording the evaluation against these criteria". M10 added no dependency at all, so
  this did not land here — see the audit below.

## What Complete needs, audited requirement by requirement at M10's close

`delivery-roadmap` defines **Complete** as *"every requirement in the capability's spec is
satisfied, every scenario has a corresponding test or documented reason, and the capability's gates
are in continuous integration"*. M10 task 6.2 planned this row for Complete and named two blockers:
confidentiality and conflict resolution. **One of the two closed here. Reading the specification
requirement by requirement against this tree finds six more that no amount of finishing those two
would have covered** — which is the finding rather than the excuse, and it is why the row is
recorded at Working for the second consecutive milestone.

| # | Requirement | State | Evidence |
|---|---|---|---|
| 1 | A save is the overlay, not a second model | **satisfied at M11.e** | `Overlay` is the one structure, and `cy::world-persistence` (`src/world/persistence/`) is the one translation from `world::PersistenceOverlay` into it — above this module, linking `cy::save`, and descriptor-driven from the ECS component registry. `samples/06-open-world` calls it and keeps no copy; `unit.world_persistence` round-trips a world overlay through a committed generation and compares |
| 2 | Persistence scopes | satisfied | `identity.h`; `tests/test_overlay.cpp` |
| 3 | Persistence traits | satisfied | `traits.h`; `tests/test_traits.cpp`, seven cases including the table that "cannot drift apart" |
| 4 | Persistent identity | **partial** | identities are stable and never derived; `LoadFailure::UnresolvableReference` is declared, **produced by nothing**, and the scenario "references into unloaded regions" has no test — nothing in this module resolves a reference |
| 5 | Entity deltas and tombstones | satisfied | `tests/test_overlay.cpp`, five cases |
| 6 | Dirty tracking | **partial** | the model tracks and survives unloading; `SaveService` does not clear the flags it captured — see above |
| 7 | Saving an unloaded world | satisfied | "residency does not change what a save contains, byte for byte" |
| 8 | The save journal | satisfied | `tests/test_archive.cpp`, append, replay and compaction |
| 9 | Consistent snapshots and background writing | **partial** | the behaviour holds and is tested; the MECHANISM the specification names — versioning or copy-on-write of the chunks — is not what `service.cpp` does |
| 10 | Save container and manifest | satisfied | `tests/test_container.cpp` |
| 11 | Atomic writes and generations | satisfied | `tests/test_kill_nine.cpp`, a real `SIGKILL` at each of the five phases |
| 12 | The load pipeline | **partial** | every ingredient exists, and the save-to-world step is `world::from_save_overlay` in `cy::world-persistence` since M11.e; the ORDERED pipeline the requirement lists is still assembled by the caller — `samples/06-open-world/` — and "apply persistent deltas as cells activate" is `world::CellActivation` over the world overlay that translation fills |
| 13 | Compatibility and migration | satisfied | `tests/test_container.cpp`, migration chains and five structured refusals |
| 14 | Plugin-owned state | **partial** | per-save inventory, not per-record ownership — see above |
| 15 | Integrity and confidentiality | **NOT SATISFIED** | integrity is complete; **confidentiality is absent**, and closing it is a dependency decision (mbedTLS) rather than a coding task |
| 16 | Storage backends and the cloud boundary | **satisfied at M10** | backends were already here; `conflict.h` and its eight cases are task 6.2's half that landed |
| 17 | Checkpoints and restore | **partial** | memory-only; "in memory **as well as** on storage" is half-built |
| 18 | Save diagnostics and inspection | **satisfied at M11.e** | `inspect.h`: `inspect_save`, `explain_fields`, `diff_saves`, and `cy_save_inspect` / `just diagnose-save` over them; `tests/test_inspect.cpp`, seven cases over the three-generation fixture campaign `tests/inspect_fixture.h` writes, and two in `tests/test_archive.cpp` over its committed copy (`tests/data/inspect-campaign/`) |
| 19 | Save performance and testing | **partial** | transactional, fuzz and migration testing are all here; the **large-world save benchmark** is `benchmarks/save/` since M11.e — one autosave over 1 048 576 persistent objects and over 65 536, the same 20 480 dirty records in each, thresholds in `benchmarks/baseline.json` and `m11a:save-benchmark` requiring the two within 1.5x — and there are still no save-as-fixture tests |
| 20 | Forbidden save patterns | **checked at M11.e** | `tools/save/check_forbidden.py`: a static check over save code for seven of the ten, a `forbidden save pattern <id>:` case in `tests/test_forbidden.cpp` for nine; `tools/save/selftest.py` plants each static pattern in a copy of the tree and requires it reported |

**Nine of the twenty are satisfied. Three are outright unmet — 15, 18 and 20 — and eight more are
partial, one of which (19) is missing an artefact the requirement says the engine SHALL maintain.**
Confidentiality is one line of that table. A milestone that plans this row for Complete has to plan
the other ten as well, and M11's proposal is where that belongs.

**M11.e moved two rows, and the counts above are M10's.** Row 1 is satisfied — ten of the twenty —
now that `cy::world-persistence` is the engine's translation into this module; row 19 has its
benchmark and is still partial for want of save-as-fixture tests. Both rows are the closing of
`m11a:save-has-an-engine-consumer` and `m11a:save-benchmark` (OpenSpec change
`close-save-system-gaps`).

**Rows 18 and 20 moved too.** Row 18 is satisfied — eleven of the twenty — and row 20 is checked;
they close `m11a:save-inspector` and `m11a:save-forbidden-patterns-checked` in the same change. See
the next two sections.

## The save inspector (M11.e)

`inspect.h` answers the three questions `Save diagnostics and inspection` asks, and
`cy_save_inspect` (`tools/save/inspect/`, `just diagnose-save`) prints the answers one fact per line:

* **What is in a save**: `inspect_save()` — the manifest, the retained generations, entity counts,
  and size by scope, region, component and plugin (a component's owning module is `TypeInfo::module`).
  Sizes are the container's own encoder run over what was loaded, so for a generation with no
  journal they equal the manifest's chunk sizes exactly, and a case holds them to it.
* **Why a field is there, and why a save was not restored**: `explain_fields()` names the component,
  field, persistence trait, module, entity and **the generation it has held its value since**. A
  save records no per-field clock — adding one would put a timestamp into content-addressed bytes —
  so "when it became dirty" is read off the retained generations, and `<=` in the output says the
  history ran out before the value changed. `SaveInspection::restore` is what a load under the
  caller's policy reports, and the contents are then read under a permissive one, so a save this
  build refuses can still be looked at.
* **What changed**: `diff_saves()` merges the two overlays' sorted sequences — regions, entities,
  components, fields, fragments — and reports in stable identifiers only. Equal logical state in
  different bytes diffs empty, and a case commits exactly that to prove it.

**It reads through the loader, never beside it.** `SaveArchive::load_generation` is public for this,
so the inspector cannot disagree with a load about what a save contains. The fixture it is tested
against is `tests/data/inspect-campaign/`, three generations written by `tests/inspect_fixture.h` and
held byte-identical to it by a case; `inspect_fixture.h` says how to regenerate it.

**`LoadReport::subject` is new, and it fixed a defect.** `detail` used to point at the build identity
or plugin name inside the manifest the load had read — and `SaveArchive::load` reads that manifest
into a local, so every refused load handed back a dangling pointer. `a refused load still names the
build that wrote the save after the load returns` (in `test_archive.cpp`) printed garbage before the
fix. `detail` is now always a literal, and what a failure names — build, plugin and version, or the
type and schema version a migration could not carry — is copied into `subject`.

## The forbidden save patterns (M11.e)

`tools/save/check_forbidden.py --list` prints the ten, in the specification's order, with how each
is checked. **Static** where the pattern is a shape of code: every production file under `src/save/`
or including a `<cy/save/...>` header, comments and literals blanked. **Runtime** where it is a
property of what a save does: the `forbidden save pattern <id>:` cases in `tests/test_forbidden.cpp`,
which the checker runs by id and requires to be selected and to pass. Without `--binary` the checker
fails rather than reporting the static half as the whole. `just diagnose-save-patterns` runs both.

| id | static | runtime |
|---|---|---|
| raw-memory | a save sink given `sizeof` a class or an address cast to bytes | no native image or unsaved field value in the chunk |
| runtime-identity | a `PersistentId` built from an index, slot, row or pointer; save code including the ECS or holding `uintptr_t` | — |
| derived-saved | a save record built for a purpose other than `Persistence` | no derived or transient value in any committed chunk |
| whole-world-resident | save code other than the overlay and the inspector reading residency | nothing resident and everything resident commit identical chunks |
| destructive-in-place | a file opened for writing rather than `fs::write_atomic` | a commit, a journal append and a compaction replace no object but the pointer |
| monolithic-blob | — | profile, campaign and session each decode alone from their own chunk |
| cooked-migration | save code including the package or cook writers, or calling a package migration | other cooked content is refused and the store is left byte-identical |
| bespoke-crypto | cryptography without mbedTLS, a key mixed in by hand, a decryption used as a verdict | a corrupt chunk is found by its content hash, and named |
| boolean-load | a load, restore, decode or inspect declared returning `bool` | loads return `Status`, and each failure has a named reason |
| invented-state | — | an unrecoverable load leaves the overlay empty; a fallback equals the older save exactly |

`tools/save/selftest.py` plants each static pattern in a copy of the real tree and requires it
reported, leaves prose naming every trigger word alone, and requires an empty tree to be refused. The
checker's first run over this tree found one real instance — `cy_save_inspect`'s own inspect helper
returned `bool` — and it returns a `Status` now.

**The runtime cases found a defect.** `SaveArchive::load` cleared the caller's overlay before each
attempt but not after the last one failed, so a load whose last chunk was corrupt handed back every
chunk it had applied before the damage: part of a save, restored as if it were one. `forbidden save
pattern invented-state: an unrecoverable load leaves nothing behind` was red before the fix. Deleting
the two lines marked `a failed load restores nothing` is `m11a:save-forbidden-patterns-checked`'s
declared mutation, and it turns the checker red on `invented-state`; deleting `++usage.modified;` in
`inspect.cpp`, `m11a:save-inspector`'s, fails three of its seven cases and the tool's counts line.
