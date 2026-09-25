#pragma once
// SPDX-License-Identifier: MIT
// The translation between the world's persistence overlay and a save's. M11.e, closing
// `m11a:save-has-an-engine-consumer`.
//
// THERE ARE TWO OVERLAY MODELS IN THIS TREE AND THIS IS THE ONE PLACE THEY MEET.
//
//   world::PersistenceOverlay   what the world maintains: keyed by `CellId`, holding the raw bytes
//                               of a component addressed by the runtime's dense
//                               `ecs::ComponentTypeId`. Applied during cell activation.
//   save::Overlay               what a save is: keyed by `RegionKey`, holding per-field
//                               `serialize::ValueRecord`s addressed by `reflect::TypeId`. Encoded,
//                               journalled and committed atomically by src/save/.
//
// Both headers cite the same requirement, and neither can absorb the other without breaking its own
// layer: src/save/ is layer 2 and may not know an ECS component id, and src/world/'s overlay is
// read on the activation path and may not pay for a value record per component. So the translation
// is a module of its own, above both, and the only one in the engine. It lived in
// samples/06-open-world until M11.e, where it could describe exactly one component type because it
// named the descriptor; here the descriptor comes from the ECS component registry, which already
// maps every reflected component id to its `reflect::TypeInfo`, so it describes every component a
// game registers.
//
// WHAT CROSSES, AND WHAT IS REFUSED RATHER THAN DROPPED.
//
//   world                         save
//   ----------------------------  ---------------------------------------------------------------
//   removed persistent entity     tombstone (`EntryKind::Tombstone`)
//   component override            `EntryKind::Modified` + one value record per component, built for
//                                 `Purpose::Persistence`, so a Derived or Transient field cannot
//                                 reach the save
//   created entity, position,     REFUSED with `ErrorCode::NotImplemented`, naming the cell.
//   subsystem blob, layer state,  A translation that silently dropped them would be a save that
//   world variable                loses state and reports success, which is worse than no save.
//
// On the way back a component record is applied over a DEFAULT-CONSTRUCTED value of its type — the
// type's own `construct` thunk, or zero bytes for a trivially relocatable type that declares none —
// so a field the save does not carry holds its default, which is what `save-and-persistence` means
// by "derived state is reconstructed after load". A record of a type this world's registry does not
// know is left out and COUNTED (`TranslationReport::unknown_types`), because a disabled plugin's
// state is the caller's decision and not this module's.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/serialize/migration.h>
#include <cy/ecs/component.h>
#include <cy/save/overlay.h>
#include <cy/world/overlay.h>

namespace cy::world {

/// The schema version recorded for a component type no `SchemaRegistry` declares. One, because a
/// migration chain starts at the version a record was written against and zero reads as "never
/// written" in a manifest.
inline constexpr u16 kDefaultSchemaVersion = 1;

/// What the translation reads its descriptors from. Borrowed; must outlive the call.
struct SaveTranslation {
    /// Required. Maps each `ecs::ComponentTypeId` the world overlay records to its reflected type.
    const ecs::ComponentRegistry* components = nullptr;
    /// Optional. The schema version recorded for each type; `kDefaultSchemaVersion` when null or
    /// when the registry does not declare the type.
    const serialize::SchemaRegistry* schemas = nullptr;
    /// Translate only `PersistenceOverlay::dirty_cells()` — the autosave's capture, recording into
    /// a `save::Overlay` that already holds the rest. False translates every cell: a full save, or
    /// the first capture into an empty overlay.
    bool dirty_cells_only = false;
};

/// What one translation did. Counted in both directions, so a round trip can be compared by number
/// as well as by value.
struct TranslationReport {
    u32 regions = 0;
    u32 removals = 0;
    u32 overrides = 0;
    /// Load only: component records whose type this world's registry does not know, left out.
    u32 unknown_types = 0;
};

/// Record everything `world` holds into `out` — or its dirty cells only, see `SaveTranslation` —
/// cell by cell in ascending `CellId` order. Does not clear either overlay's dirty flags: that is
/// the caller's, once the save it feeds has committed.
///
/// Fails without a partial promise: on an error `out` may hold the cells before the one named, and
/// the caller discards it. Refuses — `ErrorCode::NotImplemented` — a cell holding created entities,
/// positions or blobs, and an overlay holding layer states or world variables, because the save
/// model has no lossless home for them yet (src/world/persistence/README.md).
[[nodiscard]] Status to_save_overlay(const PersistenceOverlay& world,
                                     const SaveTranslation& translation, save::Overlay& out,
                                     TranslationReport* report = nullptr) noexcept;

/// Apply a loaded save's regions to `out`: tombstones become removals and component records become
/// overrides of the component the registry maps their type to.
///
/// Refuses a `Created` entry and a persistent identifier whose high word is set, because the world
/// addresses entities by 64 bits and truncating one would apply a record to a different entity.
[[nodiscard]] Status from_save_overlay(const save::Overlay& saved,
                                       const SaveTranslation& translation, PersistenceOverlay& out,
                                       TranslationReport* report = nullptr) noexcept;

}  // namespace cy::world
