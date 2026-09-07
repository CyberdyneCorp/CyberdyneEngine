// The persistence overlay. Tasks 6.1 and 6.4.
//
// Every container here is kept SORTED and every insertion is a binary search followed by one
// insert. That is not a performance choice — a hash table would be faster to build — it is what
// makes two overlays holding the same logical state encode to identical bytes, which is the
// "residency does not change the result" scenario and the precondition for a content-addressed
// chunk store to recognise an unchanged region.

#include <cy/save/overlay.h>

#include <algorithm>
#include <utility>

namespace cy::save {
namespace {

/// The index at which `key` sits or would sit. The one search shape, written three times over three
/// key types rather than once over a comparator, because a comparator template here would be longer
/// than the three loops it replaced.
template <class T, class Key, class Less>
usize lower_bound(Span<const T> items, const Key& key, Less less) noexcept {
    usize low = 0;
    usize high = items.size();
    while (low < high) {
        const usize middle = low + ((high - low) / 2);
        if (less(items[middle], key)) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

usize region_index(Span<const Region> regions, RegionKey key) noexcept {
    return lower_bound(regions, key,
                       [](const Region& region, RegionKey wanted) { return region.key < wanted; });
}

usize entry_index(Span<const Entry> entries, PersistentId id) noexcept {
    return lower_bound(entries, id,
                       [](const Entry& entry, PersistentId wanted) { return entry.id < wanted; });
}

usize component_index(Span<const ComponentDelta> components, reflect::TypeId type) noexcept {
    return lower_bound(components, type,
                       [](const ComponentDelta& component, reflect::TypeId wanted) {
                           return component.type < wanted;
                       });
}

/// Fragments sort by scope first and by type second, so one scope's fragments are contiguous.
struct FragmentKey {
    Scope scope = Scope::World;
    reflect::TypeId type;
};

usize fragment_index(Span<const Fragment> fragments, FragmentKey key) noexcept {
    return lower_bound(fragments, key, [](const Fragment& fragment, const FragmentKey& wanted) {
        if (fragment.scope != wanted.scope) {
            return static_cast<u8>(fragment.scope) < static_cast<u8>(wanted.scope);
        }
        return fragment.type < wanted.type;
    });
}

/// Insert `value` at `index`, moving the tail up by one. `Array` has no positional insert because
/// almost nothing in the engine wants one; the four call sites here do, and they all want it for
/// the same reason.
template <class T>
Status insert_at(Array<T>& items, usize index, T value) noexcept {
    if (Status pushed = items.push_back(std::move(value)); !pushed) {
        return pushed;
    }
    for (usize position = items.size() - 1; position > index; --position) {
        T moved = std::move(items[position - 1]);
        items[position - 1] = std::move(items[position]);
        items[position] = std::move(moved);
    }
    return ok();
}

}  // namespace

const char* entry_kind_name(EntryKind kind) noexcept {
    switch (kind) {
        case EntryKind::Modified:
            return "modified";
        case EntryKind::Created:
            return "created";
        case EntryKind::Tombstone:
            return "tombstone";
    }
    return "unknown";
}

// --- Lookup -----------------------------------------------------------------------------------

const Region* Overlay::find_region(RegionKey region) const noexcept {
    const Span<const Region> regions = regions_.span();
    const usize index = region_index(regions, region);
    if (index >= regions.size() || regions[index].key != region) {
        return nullptr;
    }
    return &regions[index];
}

const Entry* Overlay::find_entry(RegionKey region, PersistentId id) const noexcept {
    const Region* found = find_region(region);
    if (found == nullptr) {
        return nullptr;
    }
    const Span<const Entry> entries = found->entries.span();
    const usize index = entry_index(entries, id);
    if (index >= entries.size() || entries[index].id != id) {
        return nullptr;
    }
    return &entries[index];
}

const ComponentDelta* Overlay::find_component(RegionKey region, PersistentId id,
                                              reflect::TypeId type) const noexcept {
    const Entry* entry = find_entry(region, id);
    if (entry == nullptr) {
        return nullptr;
    }
    const Span<const ComponentDelta> components = entry->components.span();
    const usize index = component_index(components, type);
    if (index >= components.size() || components[index].type != type) {
        return nullptr;
    }
    return &components[index];
}

const Fragment* Overlay::find_fragment(Scope scope, reflect::TypeId type) const noexcept {
    const Span<const Fragment> fragments = fragments_.span();
    const usize index = fragment_index(fragments, FragmentKey{scope, type});
    if (index >= fragments.size() || fragments[index].scope != scope ||
        fragments[index].type != type) {
        return nullptr;
    }
    return &fragments[index];
}

// --- Insertion --------------------------------------------------------------------------------

Expected<Region*, Error> Overlay::region_for(RegionKey region) noexcept {
    const usize index = region_index(regions_.span(), region);
    if (index < regions_.size() && regions_[index].key == region) {
        return &regions_[index];
    }
    Region fresh(*allocator_);
    fresh.key = region;
    if (Status inserted = insert_at(regions_, index, std::move(fresh)); !inserted) {
        return make_unexpected(inserted.error());
    }
    return &regions_[index];
}

Expected<Entry*, Error> Overlay::entry_for(RegionKey region, PersistentId id) noexcept {
    Expected<Region*, Error> owner = region_for(region);
    if (!owner) {
        return make_unexpected(owner.error());
    }
    Region& target = **owner;
    const usize index = entry_index(target.entries.span(), id);
    if (index < target.entries.size() && target.entries[index].id == id) {
        return &target.entries[index];
    }
    Entry fresh(*allocator_);
    fresh.id = id;
    if (Status inserted = insert_at(target.entries, index, std::move(fresh)); !inserted) {
        return make_unexpected(inserted.error());
    }
    return &target.entries[index];
}

// --- Recording --------------------------------------------------------------------------------

Status Overlay::record_component(RegionKey region, PersistentId id, const reflect::TypeInfo& type,
                                 const void* object, u16 schema_version) noexcept {
    serialize::ValueRecord record(*allocator_);
    if (Status built =
            serialize::record_from_object(type, object, serialize::Purpose::Persistence, record);
        !built) {
        return built;
    }
    return record_component(region, id, type.id, schema_version, record);
}

Status Overlay::record_component(RegionKey region, PersistentId id, reflect::TypeId type,
                                 u16 schema_version,
                                 const serialize::ValueRecord& record) noexcept {
    if (id.is_nil() || !type.valid()) {
        return fail(ErrorCode::InvalidArgument,
                    "a persistent record needs a persistent id and a type id");
    }
    Expected<Entry*, Error> entry = entry_for(region, id);
    if (!entry) {
        return make_unexpected(entry.error());
    }
    Entry& target = **entry;
    if (target.kind == EntryKind::Tombstone) {
        return fail(ErrorCode::InvalidArgument,
                    "a destroyed entity is not written to; its tombstone is the whole delta");
    }

    const usize index = component_index(target.components.span(), type);
    const bool present = index < target.components.size() && target.components[index].type == type;
    if (!present) {
        ComponentDelta fresh(*allocator_);
        fresh.type = type;
        if (Status inserted = insert_at(target.components, index, std::move(fresh)); !inserted) {
            return inserted;
        }
    }
    ComponentDelta& component = target.components[index];
    component.schema_version = schema_version;
    // Overlaid rather than replaced: a second write of one field must not drop the fields the
    // first one recorded, which is what makes a component that is written field by field over
    // several frames end up whole.
    if (Status merged = component.record.overlay(record); !merged) {
        return merged;
    }
    component.record.set_type(type);
    component.record.set_schema_version(schema_version);

    target.dirty = true;
    Region* owner = &regions_[region_index(regions_.span(), region)];
    owner->dirty = true;
    return ok();
}

Status Overlay::create_entity(RegionKey region, PersistentId id, AssetId template_asset,
                              PersistentId owner) noexcept {
    if (id.is_nil()) {
        return fail(ErrorCode::InvalidArgument, "a created entity needs a persistent id");
    }
    Expected<Entry*, Error> entry = entry_for(region, id);
    if (!entry) {
        return make_unexpected(entry.error());
    }
    Entry& target = **entry;
    if (target.kind == EntryKind::Tombstone) {
        return fail(ErrorCode::AlreadyExists,
                    "that persistent id is tombstoned in this region; identities are not reused");
    }
    target.kind = EntryKind::Created;
    target.template_asset = template_asset;
    target.owner = owner;
    target.dirty = true;
    regions_[region_index(regions_.span(), region)].dirty = true;
    return ok();
}

Status Overlay::destroy_entity(RegionKey region, PersistentId id) noexcept {
    Expected<Entry*, Error> entry = entry_for(region, id);
    if (!entry) {
        return make_unexpected(entry.error());
    }
    Region& owner = regions_[region_index(regions_.span(), region)];
    owner.dirty = true;

    Entry& target = **entry;
    if (target.kind == EntryKind::Created) {
        // It existed only because this overlay said so. Saying nothing is the correct delta, and
        // leaving a tombstone would make a save that resurrects and re-destroys it on every load.
        const usize index = entry_index(owner.entries.span(), id);
        owner.entries.erase(index);
        return ok();
    }
    target.kind = EntryKind::Tombstone;
    target.template_asset = AssetId();
    target.owner = PersistentId();
    target.components.clear();
    target.dirty = true;
    return ok();
}

Status Overlay::record_fragment(Scope scope, const reflect::TypeInfo& type, const void* object,
                                u16 schema_version) noexcept {
    serialize::ValueRecord record(*allocator_);
    if (Status built =
            serialize::record_from_object(type, object, serialize::Purpose::Persistence, record);
        !built) {
        return built;
    }
    return record_fragment(scope, type.id, schema_version, record);
}

Status Overlay::record_fragment(Scope scope, reflect::TypeId type, u16 schema_version,
                                const serialize::ValueRecord& record) noexcept {
    if (!type.valid()) {
        return fail(ErrorCode::InvalidArgument, "a scope fragment needs a type id");
    }
    const usize index = fragment_index(fragments_.span(), FragmentKey{scope, type});
    const bool present = index < fragments_.size() && fragments_[index].scope == scope &&
                         fragments_[index].type == type;
    if (!present) {
        Fragment fresh(*allocator_);
        fresh.scope = scope;
        fresh.type = type;
        if (Status inserted = insert_at(fragments_, index, std::move(fresh)); !inserted) {
            return inserted;
        }
    }
    Fragment& fragment = fragments_[index];
    fragment.schema_version = schema_version;
    if (Status merged = fragment.record.overlay(record); !merged) {
        return merged;
    }
    fragment.record.set_type(type);
    fragment.record.set_schema_version(schema_version);
    fragment.dirty = true;
    return ok();
}

// --- Dirty tracking ---------------------------------------------------------------------------

Status Overlay::dirty_regions(Array<RegionKey>& out) const noexcept {
    out.clear();
    for (const Region& region : regions_) {
        if (!region.dirty) {
            continue;
        }
        if (Status pushed = out.push_back(region.key); !pushed) {
            return pushed;
        }
    }
    return ok();
}

usize Overlay::dirty_region_count() const noexcept {
    usize count = 0;
    for (const Region& region : regions_) {
        count += region.dirty ? 1U : 0U;
    }
    return count;
}

usize Overlay::dirty_entry_count() const noexcept {
    usize count = 0;
    for (const Region& region : regions_) {
        for (const Entry& entry : region.entries) {
            count += entry.dirty ? 1U : 0U;
        }
    }
    for (const Fragment& fragment : fragments_) {
        count += fragment.dirty ? 1U : 0U;
    }
    return count;
}

void Overlay::clear_dirty() noexcept {
    for (Region& region : regions_) {
        region.dirty = false;
        for (Entry& entry : region.entries) {
            entry.dirty = false;
        }
    }
    for (Fragment& fragment : fragments_) {
        fragment.dirty = false;
    }
}

// --- Residency --------------------------------------------------------------------------------

Status Overlay::set_residency(RegionKey region, Residency residency) noexcept {
    Expected<Region*, Error> target = region_for(region);
    if (!target) {
        return make_unexpected(target.error());
    }
    (**target).residency = residency;
    return ok();
}

Residency Overlay::residency_of(RegionKey region) const noexcept {
    const Region* found = find_region(region);
    return found == nullptr ? Residency::Unloaded : found->residency;
}

usize Overlay::resident_region_count() const noexcept {
    usize count = 0;
    for (const Region& region : regions_) {
        count += region.residency == Residency::Resident ? 1U : 0U;
    }
    return count;
}

// --- Inventory --------------------------------------------------------------------------------

usize Overlay::entry_count() const noexcept {
    usize count = 0;
    for (const Region& region : regions_) {
        count += region.entries.size();
    }
    return count;
}

usize Overlay::count_of_kind(EntryKind kind) const noexcept {
    usize count = 0;
    for (const Region& region : regions_) {
        for (const Entry& entry : region.entries) {
            count += entry.kind == kind ? 1U : 0U;
        }
    }
    return count;
}

// --- Copying and merging ----------------------------------------------------------------------

Status Overlay::clone_into(Overlay& out) const noexcept {
    out.clear();
    out.content_version_ = content_version_;
    out.simulation_point_ = simulation_point_;
    if (Status merged = out.merge(*this); !merged) {
        return merged;
    }
    // A clone carries the dirty state too: the capture that takes one is what will later be told
    // the changes were committed, and a clone that came back clean would lose them on a crash.
    //
    // Copied by KEY rather than by index. `merge` records state, and a region that holds no entries
    // — one that only ever had its residency noted — is not one it creates, so the two arrays are
    // not guaranteed to line up and an index walk would read the wrong region's flags.
    for (const Region& region : regions_) {
        Expected<Region*, Error> target = out.region_for(region.key);
        if (!target) {
            return make_unexpected(target.error());
        }
        (**target).residency = region.residency;
        (**target).dirty = region.dirty;
        for (const Entry& entry : region.entries) {
            Expected<Entry*, Error> copy = out.entry_for(region.key, entry.id);
            if (!copy) {
                return make_unexpected(copy.error());
            }
            (**copy).dirty = entry.dirty;
        }
    }
    for (const Fragment& fragment : fragments_) {
        Fragment* copy = out.mutable_fragment(fragment.scope, fragment.type);
        if (copy != nullptr) {
            copy->dirty = fragment.dirty;
        }
    }
    return ok();
}

Fragment* Overlay::mutable_fragment(Scope scope, reflect::TypeId type) noexcept {
    const usize index = fragment_index(fragments_.span(), FragmentKey{scope, type});
    if (index >= fragments_.size() || fragments_[index].scope != scope ||
        fragments_[index].type != type) {
        return nullptr;
    }
    return &fragments_[index];
}

Status Overlay::merge(const Overlay& other) noexcept {
    for (const Region& region : other.regions_) {
        for (const Entry& entry : region.entries) {
            if (entry.kind == EntryKind::Tombstone) {
                if (Status destroyed = destroy_entity(region.key, entry.id); !destroyed) {
                    return destroyed;
                }
                continue;
            }
            if (entry.kind == EntryKind::Created) {
                if (Status created =
                        create_entity(region.key, entry.id, entry.template_asset, entry.owner);
                    !created) {
                    return created;
                }
            }
            for (const ComponentDelta& component : entry.components) {
                if (Status recorded = record_component(region.key, entry.id, component.type,
                                                       component.schema_version, component.record);
                    !recorded) {
                    return recorded;
                }
            }
        }
    }
    for (const Fragment& fragment : other.fragments_) {
        if (Status recorded = record_fragment(fragment.scope, fragment.type,
                                              fragment.schema_version, fragment.record);
            !recorded) {
            return recorded;
        }
    }
    simulation_point_ = std::max(other.simulation_point_, simulation_point_);
    return ok();
}

void Overlay::clear() noexcept {
    regions_.clear();
    fragments_.clear();
    content_version_ = assets::ContentHash();
    simulation_point_ = 0;
}

}  // namespace cy::save
