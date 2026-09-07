#pragma once
// The persistence overlay: the delta against authored content that IS the save. Tasks 6.1 and 6.4.
//
// `save-and-persistence` — "A save is the overlay, not a second model":
//
//     authored content + persistence overlay + scoped fragments = saved game
//
// and "a second persistence model SHALL NOT exist. Two models would be two sources of truth about
// whether a bridge is destroyed". `world-partition-and-streaming` — "Persistence overlay" — defines
// the same structure from the other side and hands the encoding, journalling, atomicity,
// incremental writing and migration to this capability. This header is where the two meet: the
// world holds one of these and publishes it into the ECS during cell activation, and a save is this
// structure encoded. There is no serialisation path beside it (M6 design.md §4).
//
// EVERYTHING IS A DELTA.
//
//   state                       recorded
//   --------------------------  ----------------------------------------------------------------
//   unchanged authored entity   nothing
//   modified authored entity    its changed persistent fields, as a value record per component
//   destroyed authored entity   a tombstone, and its field values are dropped
//   runtime-created entity      its template, its owner, and its persistent overrides
//
// An entity that has not changed contributes nothing, which is what makes the save's size a
// function of what the player did rather than of the size of the world.
//
// THE STORE IS ORGANISED BY REGION, AND THAT IS THE WHOLE ARGUMENT FOR THE EXIT CRITERION.
// "Producing a save SHALL NOT require loading regions that are not resident" is not a promise made
// by the save path; it is a consequence of the store being keyed by `RegionKey` and holding value
// records rather than live entities. A region's persistent state is reachable with the region
// unloaded because the store never held anything that unloading takes away — which is also why
// `set_residency()` below is bookkeeping for the inspector and is read by nothing that writes a
// save. src/save/tests/test_overlay.cpp saves the same logical state with different regions
// resident and requires the bytes to be equal.
//
// ORDER IS BY KEY, NOT BY INSERTION. Regions are held sorted by `RegionKey`, entries within a
// region sorted by `PersistentId`, components within an entry sorted by `TypeId`, and a value
// record already sorts its fields by `FieldId`. Two overlays holding the same logical state
// therefore encode to identical bytes whatever order they were built in — the "residency does not
// change the result" scenario, and the property a content-addressed chunk store rests on, since a
// region whose state did not change must hash to the chunk that is already there.
//
// WHAT IS NOT HERE. Nothing derived: "spatial indexes, cached transforms, GPU scene contents,
// residency state, shadow pages, navigation working data, temporal history, and particle state are
// reconstructed after load". The classification table enforces it — `record_component()` builds its
// value record for `Purpose::Persistence`, and a `Derived` or `Transient` field is absent from that
// column — so a caller cannot put derived data in here by passing the wrong flag, because there is
// no flag to pass.

#include <cy/core/assets/hash.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/type_info.h>
#include <cy/core/serialize/value_record.h>
#include <cy/core/values/asset_id.h>
#include <cy/save/identity.h>

namespace cy::save {

/// What an entry says about its entity. Persistent: written into chunks, so an enumerator is added
/// at the end and never renumbered.
enum class EntryKind : u8 {
    /// An authored entity whose persistent fields changed.
    Modified = 0,
    /// An entity that authored content does not contain: a template, an owner, and overrides.
    Created = 1,
    /// An authored entity that was destroyed. Carries no field values, by construction.
    Tombstone = 2,
};

const char* entry_kind_name(EntryKind kind) noexcept;

/// Whether a region's authored content is currently instantiated. Bookkeeping for the inspector and
/// for tests; nothing that writes a save reads it.
enum class Residency : u8 { Unloaded = 0, Resident = 1 };

/// One component's persistent fields for one entity.
struct ComponentDelta {
    reflect::TypeId type;
    /// The schema version the record was written against. The migration chain starts here.
    u16 schema_version = 0;
    serialize::ValueRecord record;

    explicit ComponentDelta(Allocator& allocator) noexcept : record(allocator) {}
};

/// One persistent entity's delta against authored content.
struct Entry {
    PersistentId id;
    EntryKind kind = EntryKind::Modified;
    /// The template a runtime-created entity was spawned from. Nil for the other two kinds.
    AssetId template_asset;
    /// The entity that owns a runtime-created one, for `OwnerManaged` lifetimes. Nil when none.
    PersistentId owner;
    /// True since the last `clear_dirty()`. What an autosave inspects instead of the world.
    bool dirty = true;
    /// Sorted by `TypeId`. Empty for a tombstone.
    Array<ComponentDelta> components;

    explicit Entry(Allocator& allocator) noexcept : components(allocator) {}
};

/// One region's persistent state, held whether or not the region is loaded.
struct Region {
    RegionKey key;
    Residency residency = Residency::Unloaded;
    bool dirty = true;
    /// Sorted by `PersistentId`.
    Array<Entry> entries;

    explicit Region(Allocator& allocator) noexcept : entries(allocator) {}
};

/// State that belongs to a scope rather than to an entity: settings, progression, world variables,
/// layer states. `save-and-persistence` calls these the scoped fragments of the equation above.
struct Fragment {
    Scope scope = Scope::World;
    reflect::TypeId type;
    u16 schema_version = 0;
    bool dirty = true;
    serialize::ValueRecord record;

    explicit Fragment(Allocator& allocator) noexcept : record(allocator) {}
};

/// The overlay. Move-only, like every owning structure in this engine.
class Overlay {
public:
    explicit Overlay(Allocator& allocator = current_allocator()) noexcept
        : allocator_(&allocator), regions_(allocator), fragments_(allocator) {}

    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;
    Overlay(Overlay&&) noexcept = default;
    Overlay& operator=(Overlay&&) noexcept = default;
    ~Overlay() = default;

    // --- Recording ------------------------------------------------------------------------------

    /// Record one component's persistent fields for `id`, walking `type` over `object`.
    ///
    /// The only route from a live component into the overlay, and it goes through
    /// `serialize::record_from_object` with `Purpose::Persistence` — so what lands here is exactly
    /// what the classification table says a save captures, and a derived field cannot reach it.
    /// Marks the entry, its region and the overlay dirty.
    [[nodiscard]] Status record_component(RegionKey region, PersistentId id,
                                          const reflect::TypeInfo& type, const void* object,
                                          u16 schema_version) noexcept;

    /// Record a component that is already a value record — a migrated record read back from a
    /// chunk, or a plugin's opaque state this build cannot describe.
    [[nodiscard]] Status record_component(RegionKey region, PersistentId id, reflect::TypeId type,
                                          u16 schema_version,
                                          const serialize::ValueRecord& record) noexcept;

    /// Note that a runtime-created entity exists, with the template it was spawned from and the
    /// entity that owns it. Its persistent overrides are recorded with `record_component` after.
    [[nodiscard]] Status create_entity(RegionKey region, PersistentId id, AssetId template_asset,
                                       PersistentId owner = PersistentId()) noexcept;

    /// Destroy an entity.
    ///
    /// An authored entity becomes a tombstone and loses its recorded field values, because a
    /// destroyed entity's fields are not state anybody restores. A runtime-created entity is
    /// REMOVED rather than tombstoned: it exists only because the overlay says so, and an overlay
    /// that both creates and destroys it should say nothing at all.
    [[nodiscard]] Status destroy_entity(RegionKey region, PersistentId id) noexcept;

    /// Record a scope fragment from a live object.
    [[nodiscard]] Status record_fragment(Scope scope, const reflect::TypeInfo& type,
                                         const void* object, u16 schema_version) noexcept;
    [[nodiscard]] Status record_fragment(Scope scope, reflect::TypeId type, u16 schema_version,
                                         const serialize::ValueRecord& record) noexcept;

    // --- Reading --------------------------------------------------------------------------------

    [[nodiscard]] const Region* find_region(RegionKey region) const noexcept;
    [[nodiscard]] const Entry* find_entry(RegionKey region, PersistentId id) const noexcept;
    [[nodiscard]] const ComponentDelta* find_component(RegionKey region, PersistentId id,
                                                       reflect::TypeId type) const noexcept;
    [[nodiscard]] const Fragment* find_fragment(Scope scope, reflect::TypeId type) const noexcept;

    /// Every region, in ascending key order. The order a save is written in.
    [[nodiscard]] Span<const Region> regions() const noexcept { return regions_.span(); }
    [[nodiscard]] Span<const Fragment> fragments() const noexcept { return fragments_.span(); }

    // --- Dirty tracking -------------------------------------------------------------------------

    /// Regions holding at least one change since the last `clear_dirty()`, in ascending key order.
    ///
    /// This is what an autosave walks. "An autosave in a world of ten million persistent objects
    /// SHALL cost work proportional to what changed, not to the size of the world" is this call
    /// being O(dirty regions) and the encoder writing only what it names.
    [[nodiscard]] Status dirty_regions(Array<RegionKey>& out) const noexcept;
    [[nodiscard]] usize dirty_region_count() const noexcept;
    [[nodiscard]] usize dirty_entry_count() const noexcept;
    [[nodiscard]] bool is_dirty() const noexcept { return dirty_entry_count() != 0; }

    /// Mark everything clean. Called once a save has been durably committed, and not before: a
    /// crash between the capture and the commit must leave the changes dirty.
    void clear_dirty() noexcept;

    // --- Residency ------------------------------------------------------------------------------

    /// Note that a region's authored content was instantiated or released. Changes no persistent
    /// state: an entry recorded while resident is identical to the same entry after the region
    /// unloads, which is the "changes survive unloading" scenario.
    [[nodiscard]] Status set_residency(RegionKey region, Residency residency) noexcept;
    [[nodiscard]] Residency residency_of(RegionKey region) const noexcept;
    [[nodiscard]] usize resident_region_count() const noexcept;

    // --- Inventory ------------------------------------------------------------------------------

    [[nodiscard]] usize region_count() const noexcept { return regions_.size(); }
    [[nodiscard]] usize entry_count() const noexcept;
    [[nodiscard]] usize count_of_kind(EntryKind kind) const noexcept;

    /// The content version the overlay was produced against: the hash of the cooked content the
    /// authored side of the equation comes from. `world-partition-and-streaming` requires an
    /// overlay to declare it "so incompatibility is detected rather than misapplied".
    void set_content_version(const assets::ContentHash& version) noexcept {
        content_version_ = version;
    }
    [[nodiscard]] const assets::ContentHash& content_version() const noexcept {
        return content_version_;
    }

    /// The simulation point the overlay is a view of: the tick at whose commit boundary it was
    /// captured. Zero until a capture sets it.
    void set_simulation_point(u64 tick) noexcept { simulation_point_ = tick; }
    [[nodiscard]] u64 simulation_point() const noexcept { return simulation_point_; }

    [[nodiscard]] Allocator& allocator() const noexcept { return *allocator_; }

    /// A deep copy. Explicit and fallible, because it allocates — and it is what a bounded capture
    /// does with the dirty half of the overlay before the background writer takes it.
    [[nodiscard]] Status clone_into(Overlay& out) const noexcept;

    /// Copy every entry of `other` over this overlay's, layering component records field by field.
    /// What applying a journal to a base checkpoint is, and what loading a save into a live overlay
    /// is: later data wins, and anything `other` does not mention keeps its value.
    [[nodiscard]] Status merge(const Overlay& other) noexcept;

    void clear() noexcept;

private:
    [[nodiscard]] Expected<Region*, Error> region_for(RegionKey region) noexcept;
    [[nodiscard]] Expected<Entry*, Error> entry_for(RegionKey region, PersistentId id) noexcept;
    [[nodiscard]] Fragment* mutable_fragment(Scope scope, reflect::TypeId type) noexcept;

    Allocator* allocator_ = nullptr;
    /// Sorted by key.
    Array<Region> regions_;
    /// Sorted by (scope, type).
    Array<Fragment> fragments_;
    assets::ContentHash content_version_;
    u64 simulation_point_ = 0;
};

}  // namespace cy::save
