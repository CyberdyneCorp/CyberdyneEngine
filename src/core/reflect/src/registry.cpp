// The registry's storage and its index. Task 1.1.1, revised for M2 task 1.1.
//
// A grown array of borrowed pointers, a parallel array of field indices, and an open-addressed
// index over both TypeId and name. The engine's own containers and its allocator interface are the
// memory module's work at M1 (`core-memory-and-containers`); this module is beneath them in the
// dependency order and cannot use them, so it uses the fewest primitives that will do and will move
// onto the allocator interface when one exists above it.
//
// A failed allocation is an OutOfMemory Status, not a throw and not an abort: registration happens
// during startup, where a host that cannot proceed still deserves to say so.
//
// The entries array stays the storage and the index is a view onto it. That is deliberate: the
// generated aggregate registers in sorted order and `begin()`/`end()` must keep reporting that
// order, because a registry whose iteration order depended on a hash function would make every
// artefact derived from a walk of it depend on one too.

#include <cy/core/reflect/registry.h>

#include <cy/core/reflect/control_plane.h>

#include <cstring>
#include <new>
#include <utility>

namespace cy::reflect {
namespace {

bool same_string(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    return std::strcmp(a, b) == 0;
}

}  // namespace

TypeRegistry::~TypeRegistry() {
    clear();
}

TypeRegistry::TypeRegistry(TypeRegistry&& other) noexcept
    : entries_(other.entries_),
      field_indices_(other.field_indices_),
      count_(other.count_),
      capacity_(other.capacity_),
      slots_(other.slots_),
      slot_count_(other.slot_count_),
      by_id_(other.by_id_),
      by_name_(other.by_name_) {
    other.entries_ = nullptr;
    other.field_indices_ = nullptr;
    other.count_ = 0;
    other.capacity_ = 0;
    other.slots_ = nullptr;
    other.slot_count_ = 0;
    other.by_id_ = detail::ProbeTable{};
    other.by_name_ = detail::ProbeTable{};
}

TypeRegistry& TypeRegistry::operator=(TypeRegistry&& other) noexcept {
    if (this != &other) {
        clear();
        entries_ = other.entries_;
        field_indices_ = other.field_indices_;
        count_ = other.count_;
        capacity_ = other.capacity_;
        slots_ = other.slots_;
        slot_count_ = other.slot_count_;
        by_id_ = other.by_id_;
        by_name_ = other.by_name_;
        other.entries_ = nullptr;
        other.field_indices_ = nullptr;
        other.count_ = 0;
        other.capacity_ = 0;
        other.slots_ = nullptr;
        other.slot_count_ = 0;
        other.by_id_ = detail::ProbeTable{};
        other.by_name_ = detail::ProbeTable{};
    }
    return *this;
}

void TypeRegistry::clear() noexcept {
    delete[] entries_;
    delete[] field_indices_;
    delete[] slots_;
    entries_ = nullptr;
    field_indices_ = nullptr;
    slots_ = nullptr;
    count_ = 0;
    capacity_ = 0;
    slot_count_ = 0;
    by_id_ = detail::ProbeTable{};
    by_name_ = detail::ProbeTable{};
}

Status TypeRegistry::rebuild_index(usize slot_count) {
    auto* slots = new (std::nothrow) u32[slot_count * 2];
    if (slots == nullptr) {
        return fail(ErrorCode::OutOfMemory, "TypeRegistry could not grow its lookup index");
    }
    delete[] slots_;
    slots_ = slots;
    slot_count_ = static_cast<u32>(slot_count);
    by_id_.adopt(slots_, slot_count_);
    by_name_.adopt(slots_ + slot_count_, slot_count_);

    const TypeInfo* const* entries = entries_;
    for (usize index = 0; index < count_; ++index) {
        const auto position = static_cast<u32>(index);
        const TypeId id = entries[index]->id;
        by_id_.insert(id.value(), position,
                      [entries, id](u32 candidate) { return entries[candidate]->id == id; });
        const char* name = entries[index]->name;
        by_name_.insert(detail::hash_name(name), position, [entries, name](u32 candidate) {
            return same_string(entries[candidate]->name, name);
        });
    }
    return ok();
}

Status TypeRegistry::grow(usize wanted) {
    if (wanted <= capacity_) {
        return ok();
    }
    usize capacity = (capacity_ == 0) ? 16 : capacity_ * 2;
    while (capacity < wanted) {
        capacity *= 2;
    }

    auto** grown = new (std::nothrow) const TypeInfo*[capacity];
    if (grown == nullptr) {
        return fail(ErrorCode::OutOfMemory, "TypeRegistry could not grow its entry table");
    }
    auto* grown_indices = new (std::nothrow) FieldIndex[capacity];
    if (grown_indices == nullptr) {
        delete[] grown;
        return fail(ErrorCode::OutOfMemory, "TypeRegistry could not grow its field indices");
    }
    for (usize index = 0; index < count_; ++index) {
        grown[index] = entries_[index];
        grown_indices[index] = std::move(field_indices_[index]);
    }
    delete[] entries_;
    delete[] field_indices_;
    entries_ = grown;
    field_indices_ = grown_indices;
    capacity_ = capacity;

    // The index is sized from the capacity rather than the count, so it is rebuilt only when the
    // entry table grows — amortised with it rather than on every registration.
    return rebuild_index(detail::table_slots_for(static_cast<u32>(capacity)));
}

Status TypeRegistry::add(const TypeInfo& info) {
    if (!info.id.valid()) {
        return fail(ErrorCode::InvalidArgument,
                    "a type registered with the null TypeId — the manifest assigns identifiers, "
                    "and zero is never assigned");
    }

    // Not find(): this is registration, not a reflected lookup, and counting it as one would make
    // startup look like a control-plane violation to anything measuring.
    const TypeInfo* const* entries = entries_;
    const TypeId id = info.id;
    const u32 existing = by_id_.find(
        id.value(), [entries, id](u32 candidate) { return entries[candidate]->id == id; });
    if (existing != detail::ProbeTable::absent) {
        const TypeInfo* found = entries_[existing];
        if (found == &info || same_string(found->name, info.name)) {
            return ok();  // the same descriptor, or the same type registered twice
        }
        return fail(ErrorCode::AlreadyExists,
                    "two different types claim one TypeId — an identifier was reused, which is the "
                    "failure the identity manifest's tombstones exist to prevent");
    }

    if (auto grown = grow(count_ + 1); !grown) {
        return grown;
    }
    if (auto built = field_indices_[count_].build(info); !built) {
        return built;
    }

    entries_[count_] = &info;
    const auto position = static_cast<u32>(count_);
    const TypeInfo* const* table = entries_;
    by_id_.insert(id.value(), position,
                  [table, id](u32 candidate) { return table[candidate]->id == id; });
    by_name_.insert(detail::hash_name(info.name), position, [table, &info](u32 candidate) {
        return same_string(table[candidate]->name, info.name);
    });
    ++count_;
    return ok();
}

const TypeInfo* TypeRegistry::find(TypeId id) const noexcept {
    detail::note_reflected_lookup();
    const TypeInfo* const* entries = entries_;
    const u32 found = by_id_.find(
        id.value(), [entries, id](u32 candidate) { return entries[candidate]->id == id; });
    return (found == detail::ProbeTable::absent) ? nullptr : entries_[found];
}

const TypeInfo* TypeRegistry::find(const char* name) const noexcept {
    detail::note_reflected_lookup();
    const TypeInfo* const* entries = entries_;
    const u32 found = by_name_.find(detail::hash_name(name), [entries, name](u32 candidate) {
        return same_string(entries[candidate]->name, name);
    });
    return (found == detail::ProbeTable::absent) ? nullptr : entries_[found];
}

const FieldIndex* TypeRegistry::fields(TypeId id) const noexcept {
    detail::note_reflected_lookup();
    const TypeInfo* const* entries = entries_;
    const u32 found = by_id_.find(
        id.value(), [entries, id](u32 candidate) { return entries[candidate]->id == id; });
    return (found == detail::ProbeTable::absent) ? nullptr : &field_indices_[found];
}

u32 TypeRegistry::longest_probe() const noexcept {
    const u32 by_id = by_id_.longest_probe();
    const u32 by_name = by_name_.longest_probe();
    return (by_id > by_name) ? by_id : by_name;
}

TypeRegistry& default_registry() {
    // Function-local, so it is constructed on first use rather than in an order the linker chose.
    static TypeRegistry registry;
    return registry;
}

}  // namespace cy::reflect
