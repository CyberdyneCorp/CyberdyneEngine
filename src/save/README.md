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

## What is not finished, and is written down rather than implied

* **The capture copies the overlay, not the world.** The specification asks for the bounded
  main-thread cost to come from versioning or copy-on-write of the chunks that hold persistent
  state. Copying the delta is bounded by what changed rather than by world size, which is the
  property the exit criterion needs; copy-on-write is a change to `service.cpp` alone.
* **Dirty flags are not cleared by the service.** Clearing exactly the records that were captured
  needs that same per-chunk versioning. Until then `take_outcome()` reports what committed and the
  caller decides, because clearing everything on completion would silently drop changes made during
  the write.
* **Plugin ownership is declared per save, not per record.** The manifest carries the plugin
  inventory and a load names the missing one; attributing an individual record to its owning module
  needs the module registry that arrives with `project-and-plugins`.
* **Encryption is absent, deliberately.** `save-and-persistence` requires that encryption never be
  presented as integrity and that no bespoke cryptography be written. Chunks carry BLAKE3 content
  hashes and the manifest carries theirs; confidentiality is an authenticated-encryption pass over
  the same objects, and it lands with a key management story rather than before one.
* **The save inspector and the semantic save diff** are specified and not built. `Manifest` already
  answers size by scope and region and the counts a load reports; the presentation and the diff are
  not this milestone's.
