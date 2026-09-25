// The translation between the world's persistence overlay and a save's. See save_translation.h.

#include <cy/world/persistence/save_translation.h>

#include <cy/core/memory/array.h>
#include <cy/core/reflect/field_index.h>
#include <cy/core/serialize/value_record.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace cy::world {

namespace {

/// The widest alignment a scratch object is given. A component aligned wider than a cache line is
/// refused rather than misaligned.
constexpr usize kScratchAlignment = 64;

/// One default-constructed object of a reflected type at a time, in a buffer reused across calls.
///
/// The world overlay's value pool packs component bytes with no alignment, and `record_from_object`
/// and `record_to_object` read and write fields at their declared offsets, so neither may be handed
/// a pointer into the pool.
class Scratch {
public:
    explicit Scratch(Allocator& allocator) noexcept : allocator_(&allocator) {}
    ~Scratch() { release(); }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    Scratch(Scratch&&) = delete;
    Scratch& operator=(Scratch&&) = delete;

    /// A default value of `type`: its `construct` thunk, or zero bytes when it declares none.
    [[nodiscard]] Expected<void*, Error> make(const reflect::TypeInfo& type) noexcept {
        if (Status reserved = reserve(type); !reserved) {
            return make_unexpected(reserved.error());
        }
        std::memset(data_, 0, type.size);
        if (type.construct != nullptr) {
            type.construct(data_);
        }
        return data_;
    }

    /// An aligned copy of a whole component's bytes. Every ECS component is trivially relocatable
    /// (`ComponentRegistry::register_reflected` refuses any other), so a copy of the bytes is a
    /// valid object and nothing needs destroying afterwards.
    [[nodiscard]] Expected<const void*, Error> copy(const reflect::TypeInfo& type,
                                                    Span<const u8> bytes) noexcept {
        if (bytes.size() != type.size) {
            return make_unexpected(Error{ErrorCode::InvalidArgument,
                                         "a world overlay override's size is not its component "
                                         "type's size",
                                         0});
        }
        if (Status reserved = reserve(type); !reserved) {
            return make_unexpected(reserved.error());
        }
        std::memcpy(data_, bytes.data(), bytes.size());
        return static_cast<const void*>(data_);
    }

    /// End the lifetime `make` began.
    static void destroy(const reflect::TypeInfo& type, void* object) noexcept {
        if (type.destruct != nullptr) {
            type.destruct(object);
        }
    }

private:
    [[nodiscard]] Status reserve(const reflect::TypeInfo& type) noexcept {
        if (type.alignment > kScratchAlignment || type.size == 0) {
            return fail(ErrorCode::Unsupported,
                        "a component of zero size or wider than 64-byte alignment cannot be "
                        "translated");
        }
        if (type.size <= capacity_) {
            return ok();
        }
        release();
        data_ = allocator_->allocate(type.size, kScratchAlignment);
        if (data_ == nullptr) {
            return fail(ErrorCode::OutOfMemory, "the translation's scratch object");
        }
        capacity_ = type.size;
        return ok();
    }

    void release() noexcept {
        allocator_->deallocate(data_, capacity_, kScratchAlignment);
        data_ = nullptr;
        capacity_ = 0;
    }

    Allocator* allocator_;
    void* data_ = nullptr;
    usize capacity_ = 0;
};

[[nodiscard]] u16 schema_version_of(const SaveTranslation& translation,
                                    reflect::TypeId type) noexcept {
    if (translation.schemas == nullptr) {
        return kDefaultSchemaVersion;
    }
    const Expected<u16, Error> declared = translation.schemas->current_version(type);
    return declared ? *declared : kDefaultSchemaVersion;
}

[[nodiscard]] bool is_removed(const CellOverlay& cell, PersistentId entity) noexcept {
    return std::ranges::any_of(cell.removed.span(),
                               [entity](PersistentId removed) { return removed == entity; });
}

/// What the save model has no lossless home for yet. Refused, never dropped.
[[nodiscard]] Status check_translatable(const CellOverlay& cell) noexcept {
    if (!cell.created.blocks.empty()) {
        return fail(ErrorCode::NotImplemented,
                    "a runtime-created entity in the world overlay has no template to save as");
    }
    if (!cell.positions.empty()) {
        return fail(ErrorCode::NotImplemented,
                    "a persistent position in the world overlay has no save representation yet");
    }
    if (!cell.blobs.empty()) {
        return fail(ErrorCode::NotImplemented,
                    "a subsystem blob in the world overlay has no save representation yet");
    }
    return ok();
}

[[nodiscard]] Status record_override(const PersistenceOverlay& world, const CellOverlay& cell,
                                     const ComponentOverride& change,
                                     const SaveTranslation& translation, save::Overlay& out,
                                     Scratch& scratch) noexcept {
    const ecs::ComponentRegistry& registry = *translation.components;
    if (!registry.registered(change.component) || registry.info(change.component).type == nullptr) {
        return fail(ErrorCode::NotFound,
                    "a world overlay override names a component with no reflected type");
    }
    const reflect::TypeInfo& type = *registry.info(change.component).type;
    Expected<const void*, Error> object = scratch.copy(type, world.override_bytes(change));
    if (!object) {
        return make_unexpected(object.error());
    }
    return out.record_component(save::RegionKey{cell.cell.value},
                                save::PersistentId{0, change.entity.value}, type, *object,
                                schema_version_of(translation, type.id));
}

[[nodiscard]] Status record_cell(const PersistenceOverlay& world, const CellOverlay& cell,
                                 const SaveTranslation& translation, save::Overlay& out,
                                 Scratch& scratch, TranslationReport& report) noexcept {
    if (Status translatable = check_translatable(cell); !translatable) {
        return translatable;
    }
    const save::RegionKey region{cell.cell.value};
    for (const PersistentId removed : cell.removed.span()) {
        if (Status destroyed = out.destroy_entity(region, save::PersistentId{0, removed.value});
            !destroyed) {
            return destroyed;
        }
        ++report.removals;
    }
    for (const ComponentOverride& change : cell.overrides.span()) {
        // A destroyed entity's fields are not state anybody restores: its tombstone is the whole
        // delta, and `save::Overlay` refuses a write to one.
        if (is_removed(cell, change.entity)) {
            continue;
        }
        if (Status recorded = record_override(world, cell, change, translation, out, scratch);
            !recorded) {
            return recorded;
        }
        ++report.overrides;
    }
    ++report.regions;
    return ok();
}

/// A `FieldIndex` per type met on the way back, so a load builds one per type rather than one per
/// record.
class FieldIndexCache {
public:
    explicit FieldIndexCache(Allocator& allocator) noexcept : entries_(allocator) {}

    [[nodiscard]] Expected<const reflect::FieldIndex*, Error> find(
        const reflect::TypeInfo& type) noexcept {
        for (const Entry& entry : entries_.span()) {
            if (entry.type == type.id) {
                return &entry.index;
            }
        }
        Entry fresh;
        fresh.type = type.id;
        if (Status built = fresh.index.build(type); !built) {
            return make_unexpected(built.error());
        }
        if (Status pushed = entries_.push_back(std::move(fresh)); !pushed) {
            return make_unexpected(pushed.error());
        }
        return &entries_[entries_.size() - 1].index;
    }

private:
    struct Entry {
        reflect::TypeId type;
        reflect::FieldIndex index;
    };
    Array<Entry> entries_;
};

struct LoadContext {
    const SaveTranslation* translation;
    PersistenceOverlay* out;
    Scratch* scratch;
    FieldIndexCache* indexes;
    TranslationReport* report;
};

[[nodiscard]] Status apply_component(const LoadContext& context, CellId cell, PersistentId entity,
                                     const save::ComponentDelta& delta) noexcept {
    const ecs::ComponentInfo* info = context.translation->components->find(delta.type);
    if (info == nullptr || info->type == nullptr) {
        ++context.report->unknown_types;
        return ok();
    }
    const reflect::TypeInfo& type = *info->type;
    Expected<const reflect::FieldIndex*, Error> fields = context.indexes->find(type);
    if (!fields) {
        return make_unexpected(fields.error());
    }
    Expected<void*, Error> object = context.scratch->make(type);
    if (!object) {
        return make_unexpected(object.error());
    }
    Status applied = serialize::record_to_object(delta.record, **fields, *object);
    if (applied) {
        applied = context.out->record_component(
            cell, entity, info->id, Span<const u8>{static_cast<const u8*>(*object), type.size});
    }
    Scratch::destroy(type, *object);
    if (applied) {
        ++context.report->overrides;
    }
    return applied;
}

[[nodiscard]] Status apply_entry(const LoadContext& context, CellId cell,
                                 const save::Entry& entry) noexcept {
    if (entry.id.high() != 0) {
        return fail(ErrorCode::OutOfRange,
                    "a saved persistent id does not fit the world's 64-bit identity");
    }
    const PersistentId entity{entry.id.low()};
    switch (entry.kind) {
        case save::EntryKind::Tombstone:
            ++context.report->removals;
            return context.out->record_removed(cell, entity);
        case save::EntryKind::Created:
            return fail(
                ErrorCode::NotImplemented,
                "a saved runtime-created entity cannot be applied to the world overlay yet");
        case save::EntryKind::Modified:
            break;
    }
    for (const save::ComponentDelta& delta : entry.components.span()) {
        if (Status applied = apply_component(context, cell, entity, delta); !applied) {
            return applied;
        }
    }
    return ok();
}

}  // namespace

Status to_save_overlay(const PersistenceOverlay& world, const SaveTranslation& translation,
                       save::Overlay& out, TranslationReport* report) noexcept {
    if (translation.components == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a save translation needs a component registry");
    }
    if (!world.layer_states().empty() || world.variable_count() != 0) {
        return fail(ErrorCode::NotImplemented,
                    "world layer states and variables have no save representation yet");
    }
    TranslationReport counted;
    Array<CellId> cells(out.allocator());
    if (Status listed =
            translation.dirty_cells_only ? world.dirty_cells(cells) : world.cells(cells);
        !listed) {
        return listed;
    }
    Scratch scratch(out.allocator());
    for (const CellId cell : cells.span()) {
        const CellOverlay* entry = world.find(cell);
        if (entry == nullptr) {
            continue;
        }
        if (Status recorded = record_cell(world, *entry, translation, out, scratch, counted);
            !recorded) {
            return recorded;
        }
    }
    if (report != nullptr) {
        *report = counted;
    }
    return ok();
}

Status from_save_overlay(const save::Overlay& saved, const SaveTranslation& translation,
                         PersistenceOverlay& out, TranslationReport* report) noexcept {
    if (translation.components == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a save translation needs a component registry");
    }
    TranslationReport counted;
    Scratch scratch(saved.allocator());
    FieldIndexCache indexes(saved.allocator());
    const LoadContext context{&translation, &out, &scratch, &indexes, &counted};
    for (const save::Region& region : saved.regions()) {
        const CellId cell{region.key.value()};
        for (const save::Entry& entry : region.entries.span()) {
            if (Status applied = apply_entry(context, cell, entry); !applied) {
                return applied;
            }
        }
        ++counted.regions;
    }
    if (report != nullptr) {
        *report = counted;
    }
    return ok();
}

}  // namespace cy::world
