#pragma once
// SPDX-License-Identifier: MIT
// The save inspector and the semantic save diff. M11.e, `close-save-system-gaps`.
//
// `save-and-persistence` — "Save diagnostics and inspection" — asks three questions of a save and
// requires an answer to each:
//
//   * WHAT IS IN IT. Manifest contents, scope inventory, counts of modified, created and tombstoned
//     entities, size by scope, region, component and plugin, and the simulation point.
//   * WHY IS THIS FIELD IN IT, naming "the component, the field, its persistence trait, the entity,
//     and when it became dirty" — and WHY WAS STATE NOT RESTORED, naming "the missing plugin,
//     failed migration, or unresolved reference".
//   * WHAT CHANGED BETWEEN TWO OF THEM, "semantically: entities created and destroyed, fields
//     changed, and scope-level differences, using stable identifiers" rather than as bytes.
//
// NOTHING HERE PARSES A SAVE. The inspector loads a generation through `SaveArchive` — the same
// manifest decode, chunk verification, migration and journal replay a game's load runs — and reads
// the `Overlay` that comes back. A second reader beside the container would be the "second
// serialisation path" src/save/CMakeLists.txt calls a defect, and an inspector that could disagree
// with the loader about what a save contains would answer the wrong question.
//
// SIZES ARE MEASURED, NOT ESTIMATED. A region's size is the length of `encode_region()`'s output
// for it, and a component's is the length of its value record as the container writes it — the
// same bytes a commit hashes. For a generation with no journal the region sizes therefore equal the
// manifest's chunk sizes exactly, and src/save/tests/test_inspect.cpp holds them to that.
//
// "WHY" COMES FROM THE SCHEMA, AND THE SCHEMA IS THE BUILD'S. A save carries identifiers and never
// names — `TypeId`, `FieldId`, `PersistentId` — so what a field is called, which persistence trait
// put it there and which module owns its type are questions only a type registry answers. The
// caller passes the one its build registered. A type the registry does not declare is reported as
// exactly that: state carried through untouched because a load preserves what it cannot interpret,
// which is itself the answer to "why is this here".
//
// "WHEN IT BECAME DIRTY" IS ANSWERED FROM THE GENERATIONS. A save does not record a timestamp per
// field, and adding one would put a clock into bytes that are content-addressed. What the store
// does retain is its generations, so a field "became dirty" at the oldest retained generation from
// which it has held its current value without a break — reported with that generation's simulation
// point. When the run reaches the oldest generation retained, the answer is "at or before" it,
// and the origin says so rather than claiming a precision the store does not have.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/ids.h>
#include <cy/core/reflect/registry.h>
#include <cy/core/serialize/wire.h>
#include <cy/save/archive.h>
#include <cy/save/container.h>
#include <cy/save/identity.h>
#include <cy/save/overlay.h>
#include <cy/save/traits.h>

namespace cy::save {

/// What the inspector explains a save against.
struct InspectOptions {
    /// The schema: component and field names, persistence traits, owning module. Null reports every
    /// type and field by identifier, as not declared by this build.
    const reflect::TypeRegistry* types = nullptr;
    /// The policy a real load would be held to. "Why was state not restored" is the answer a load
    /// under THIS policy gives; the contents are then read under a permissive one, so a save this
    /// build refuses can still be looked at.
    LoadPolicy policy;
};

/// The owner a type is attributed to when the registry does not declare it.
inline constexpr const char* kUndeclaredModule = "(not declared by this build)";

/// Size by scope: one scope's chunks, the payload records in them, and their encoded bytes.
struct ScopeUsage {
    Scope scope = Scope::World;
    u32 chunks = 0;
    u32 records = 0;
    u64 bytes = 0;
};

/// Size by region, and the entity counts a region's delta holds, by kind.
struct RegionUsage {
    RegionKey region;
    u32 modified = 0;
    u32 created = 0;
    u32 tombstoned = 0;
    u64 bytes = 0;
};

/// Size by component: every record of one type across the save, attributed to its owning module.
struct ComponentUsage {
    reflect::TypeId type;
    /// The registry's name for the type, or "" when it declares none.
    const char* name = "";
    /// `TypeInfo::module`, or `kUndeclaredModule`.
    const char* module = kUndeclaredModule;
    u32 records = 0;
    u32 fields = 0;
    u64 bytes = 0;
};

/// Size by plugin, attributed through the owning module of each record's type.
struct PluginUsage {
    const char* module = kUndeclaredModule;
    u32 records = 0;
    u64 bytes = 0;
};

/// Everything "what is in this save" answers, for one generation.
struct SaveInspection {
    explicit SaveInspection(Allocator& allocator = current_allocator()) noexcept
        : manifest(allocator),
          overlay(allocator),
          scopes(allocator),
          regions(allocator),
          components(allocator),
          plugins(allocator),
          generations(allocator) {}

    u32 generation = 0;
    Manifest manifest;
    /// What a load under `InspectOptions::policy` reported. `restore.failed()` is "this build would
    /// not restore this save", and `restore.failure` / `restore.subject` say why.
    LoadReport restore;
    /// What the permissive read of the contents reported. `contents.failed()` means the bytes could
    /// not be read at all — a corrupt or missing chunk — and `overlay` below is empty.
    LoadReport contents;
    /// The generation's state — base and journal — as a permissive load reads it.
    Overlay overlay;

    u32 modified = 0;
    u32 created = 0;
    u32 tombstoned = 0;
    u32 fragments = 0;

    /// Only the scopes that hold something, in scope order.
    Array<ScopeUsage> scopes;
    /// Ascending region order.
    Array<RegionUsage> regions;
    /// Ascending type order.
    Array<ComponentUsage> components;
    /// Ascending by module name.
    Array<PluginUsage> plugins;
    /// Every generation the store retains, ascending. The history "when it became dirty" reads.
    Array<u32> generations;

    [[nodiscard]] u64 total_bytes() const noexcept;
};

/// Inspect one generation. Zero means the active one.
[[nodiscard]] Status inspect_save(SaveArchive& archive, u32 generation,
                                  const InspectOptions& options, SaveInspection& out) noexcept;

/// Why a field is where it is in a save.
enum class FieldReason : u8 {
    /// The schema declares the field `PersistentState`: the persistence trait put it here.
    SaveGameTrait = 0,
    /// The schema declares the type but not this field — a field a newer build added or an older
    /// one removed — and the load carried it through rather than dropping it.
    PreservedUnknownField = 1,
    /// The schema does not declare the type at all — a disabled plugin's state, or a removed type —
    /// and the load carried the whole record through.
    PreservedUnknownType = 2,
    /// The schema declares the field and says a save does NOT capture it. A save carrying it was
    /// written by a build whose schema said otherwise, and this is the inspector saying so.
    NotASavedField = 3,
    /// Not a field: the entry itself. A tombstone, or a runtime-created entity's template and
    /// owner — present because the entity was destroyed or spawned, and carrying `field` invalid.
    EntryRecord = 4,
};

const char* field_reason_name(FieldReason reason) noexcept;

/// The answer to "why is this field in the save".
struct FieldOrigin {
    /// Where. `entity` is nil and `fragment` is true for a scope fragment.
    Scope scope = Scope::World;
    RegionKey region;
    PersistentId entity;
    EntryKind kind = EntryKind::Modified;
    bool fragment = false;

    /// What, by stable identifier and — when the schema declares them — by name.
    reflect::TypeId type;
    reflect::FieldId field;
    serialize::WireType wire = serialize::WireType::Bytes;
    const char* component = "";
    const char* field_name = "";
    const char* module = kUndeclaredModule;
    Trait traits = Trait::None;
    FieldReason reason = FieldReason::PreservedUnknownType;

    /// When. The oldest retained generation from which the field has held its current value, and
    /// that generation's simulation point. `since_oldest_retained` means the history ran out before
    /// the value changed, so the field became dirty at or before that generation.
    u32 dirty_since_generation = 0;
    u64 dirty_since_tick = 0;
    bool since_oldest_retained = false;
};

/// Every field an inspected generation carries, with its origin, in save order: regions ascending,
/// entities ascending, components by type, fields by identifier; then fragments.
///
/// Loads the older generations `inspection.generations` names to answer "when" — a tool's cost,
/// paid once per generation rather than once per field.
[[nodiscard]] Status explain_fields(SaveArchive& archive, const SaveInspection& inspection,
                                    const InspectOptions& options,
                                    Array<FieldOrigin>& out) noexcept;

// --- The semantic diff --------------------------------------------------------------------------

/// What one semantic difference between two saves is, each addressed by stable identifiers.
enum class DiffKind : u8 {
    /// A runtime-created entity exists in the second save and not the first.
    EntityCreated = 0,
    /// An entity is tombstoned in the second save and was not in the first, or a runtime-created
    /// entity of the first is gone from the second.
    EntityDestroyed = 1,
    /// An entity the first save recorded a delta for has none in the second: it is back to its
    /// authored state. A tombstone lifted this way is reported as this and not as a creation,
    /// because nothing was created — authored content reappeared.
    EntityReverted = 2,
    /// A field is recorded in the second save and not the first.
    FieldAdded = 3,
    /// A field recorded in the first save is not in the second: it is back to its authored value.
    FieldRemoved = 4,
    /// A field is recorded in both with different values.
    FieldChanged = 5,
    /// A scope fragment is present in only one of the two, or its fields differ. `field` names the
    /// field when the difference is one field; it is invalid when the fragment itself came or went.
    FragmentAdded = 6,
    FragmentRemoved = 7,
    FragmentChanged = 8,
    /// Manifest-level differences a player-facing summary names: the simulation point, the content
    /// the save is a delta against, the plugin inventory.
    SimulationPointChanged = 9,
    ContentVersionChanged = 10,
    PluginsChanged = 11,
};

const char* diff_kind_name(DiffKind kind) noexcept;

/// One semantic difference, addressed by stable identifiers only.
struct DiffItem {
    DiffKind kind = DiffKind::FieldChanged;
    Scope scope = Scope::World;
    RegionKey region;
    PersistentId entity;
    reflect::TypeId type;
    reflect::FieldId field;
};

/// The difference from `before` to `after`. Byte-identical saves produce an empty list; so do two
/// saves whose chunks differ in bytes but not in meaning, which is what "semantically" means.
struct SaveDiff {
    explicit SaveDiff(Allocator& allocator = current_allocator()) noexcept : items(allocator) {}

    Array<DiffItem> items;

    [[nodiscard]] usize count_of(DiffKind kind) const noexcept;
    [[nodiscard]] bool empty() const noexcept { return items.empty(); }
};

/// Compare two inspected generations: their overlays, then their manifests.
[[nodiscard]] Status diff_saves(const SaveInspection& before, const SaveInspection& after,
                                SaveDiff& out) noexcept;

/// Compare two overlays alone. What `diff_saves` is built on, and what a test that has no manifest
/// uses directly.
[[nodiscard]] Status diff_overlays(const Overlay& before, const Overlay& after,
                                   SaveDiff& out) noexcept;

// --- Text ---------------------------------------------------------------------------------------

/// The inspection as the text `cy_save_inspect` prints. Appends to `out`; no terminator.
[[nodiscard]] Status render_inspection(const SaveInspection& inspection,
                                       const InspectOptions& options, Array<char>& out) noexcept;

/// The field origins, one line each.
[[nodiscard]] Status render_origins(Span<const FieldOrigin> origins, Array<char>& out) noexcept;

/// The diff, one line per difference, names looked up in `types` when it is given.
[[nodiscard]] Status render_diff(const SaveDiff& diff, const reflect::TypeRegistry* types,
                                 Array<char>& out) noexcept;

}  // namespace cy::save
