# `src/world/persistence/` — layer 4 (`scene`)

The translation between the world's persistence overlay and a save's: `cy::world-persistence`.

**What belongs here**: `to_save_overlay()` and `from_save_overlay()` in `save_translation.h`, and
nothing else. **What does not**: an encoding (src/save/ owns it), the world's overlay model
(src/world/ owns it), and any file I/O.

**Governed by**: `save-and-persistence` ("A save is the overlay, not a second model") and
`world-partition-and-streaming` ("Persistence overlay"). Added at M11.e, closing
`m11a:save-has-an-engine-consumer`: before it, `samples/06-open-world` held the only copy of this
translation, and no engine module above src/save/ linked `cy::save`.

## Why a module of its own

| Model | Keyed by | Holds | Owner |
|---|---|---|---|
| `world::PersistenceOverlay` | `CellId`, then `world::PersistentId` | whole component bytes, by `ecs::ComponentTypeId` | src/world/, read during cell activation |
| `save::Overlay` | `RegionKey`, then `save::PersistentId` | per-field value records, by `reflect::TypeId` | src/save/, encoded and committed atomically |

src/save/ is layer 2 and may not know an ECS component id; src/world/'s `CMakeLists.txt` rules out
"file I/O and a save format". The translation needs both, so it sits above both and links both. A
consumer of the world model that never saves does not inherit `cy::save`.

**The descriptors come from the ECS component registry.** A world overlay override is addressed by
the world's dense component id; `ecs::ComponentRegistry::info(id).type` is the reflected type that
id was registered from. The sample's copy named `Structure`'s descriptor and could translate that
one type; this one translates every component a game registers, and the sample now names no type.

## What crosses, and what is refused

| World | Save | Notes |
|---|---|---|
| removed entity | tombstone | an override the world still holds for a removed entity is left out: a tombstone is the whole delta, and `save::Overlay` refuses a write to one |
| component override | `Modified` entry, one value record per component | built for `Purpose::Persistence`, so a `Derived`, `RuntimeState` or transient field cannot reach the save |
| created entity, position, subsystem blob, layer state, world variable | **refused**, `ErrorCode::NotImplemented` | the save model has no lossless home for them yet; a translation that dropped them would report success over lost state |

On load a record is applied over a **default-constructed** value of its type (the `construct` thunk,
or zero bytes), because the world overlay stores whole components and the authored value lives in a
cooked cell this module does not read. A component meant for the overlay should therefore hold
persistent state only — the sample's `Structure` does — and a field the save does not carry comes
back at its default, which is "derived state is reconstructed after load". A record of a type the
world's registry does not know is **counted** (`TranslationReport::unknown_types`) and left out; a
saved identifier wider than 64 bits is refused rather than truncated onto a different entity.

## Tests and the benchmark

* `unit.world_persistence` (`src/world/tests/test_save_translation.cpp`): every round trip commits
  to a `SaveArchive` over a `MemoryBackend` and loads back, and compares the overlay that went in
  with the one that came out record by record. `m11a:save-has-an-engine-consumer` runs the first two
  cases by name.
* `benchmarks/save/`: the large-world save benchmark `save-and-persistence` says the engine SHALL
  maintain, measured through this translation. See `benchmarks/README.md`.
