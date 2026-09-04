// Building the field index, and probing it. M2 task 1.1. See include/cy/core/reflect/field_index.h.
//
// One allocation holds both tables back to back: the FieldId table first, the name table second.
// Two allocations would be two failure paths and two frees for data with exactly one lifetime.

#include <cy/core/reflect/field_index.h>

#include <cy/core/reflect/control_plane.h>

#include <new>

namespace cy::reflect {
namespace {

bool same_string(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

}  // namespace

FieldIndex::~FieldIndex() {
    release();
}

FieldIndex::FieldIndex(FieldIndex&& other) noexcept
    : type_(other.type_),
      slots_(other.slots_),
      slot_count_(other.slot_count_),
      by_id_(other.by_id_),
      by_name_(other.by_name_) {
    other.type_ = nullptr;
    other.slots_ = nullptr;
    other.slot_count_ = 0;
    other.by_id_ = detail::ProbeTable{};
    other.by_name_ = detail::ProbeTable{};
}

FieldIndex& FieldIndex::operator=(FieldIndex&& other) noexcept {
    if (this != &other) {
        release();
        type_ = other.type_;
        slots_ = other.slots_;
        slot_count_ = other.slot_count_;
        by_id_ = other.by_id_;
        by_name_ = other.by_name_;
        other.type_ = nullptr;
        other.slots_ = nullptr;
        other.slot_count_ = 0;
        other.by_id_ = detail::ProbeTable{};
        other.by_name_ = detail::ProbeTable{};
    }
    return *this;
}

void FieldIndex::release() noexcept {
    delete[] slots_;
    slots_ = nullptr;
    slot_count_ = 0;
    type_ = nullptr;
    by_id_ = detail::ProbeTable{};
    by_name_ = detail::ProbeTable{};
}

Status FieldIndex::build(const TypeInfo& type) {
    release();

    const u32 slot_count = detail::table_slots_for(type.field_count);
    auto* slots = new (std::nothrow) u32[static_cast<usize>(slot_count) * 2];
    if (slots == nullptr) {
        return fail(ErrorCode::OutOfMemory, "FieldIndex could not allocate its slot table");
    }

    slots_ = slots;
    slot_count_ = slot_count;
    type_ = &type;
    by_id_.adopt(slots_, slot_count);
    by_name_.adopt(slots_ + slot_count, slot_count);

    const FieldInfo* fields = type.fields;
    for (u32 index = 0; index < type.field_count; ++index) {
        const FieldId id = fields[index].id;
        const bool inserted = by_id_.insert(
            id.value(), index, [fields, id](u32 candidate) { return fields[candidate].id == id; });
        if (!inserted) {
            release();
            return fail(ErrorCode::AlreadyExists,
                        "two fields of one type claim one FieldId — an identifier was reused, and "
                        "decoding would write one field's bytes into the other's storage");
        }

        // A duplicate *name* is not an error. Names are metadata and nothing addresses data by
        // one; the first field with a name wins the lookup, which is what the linear scan this
        // replaces did too.
        const char* name = fields[index].name;
        by_name_.insert(detail::hash_name(name), index, [fields, name](u32 candidate) {
            return same_string(fields[candidate].name, name);
        });
    }
    return ok();
}

const FieldInfo* FieldIndex::find(FieldId id) const noexcept {
    detail::note_reflected_lookup();
    if (type_ == nullptr) {
        return nullptr;
    }
    const FieldInfo* fields = type_->fields;
    const u32 found =
        by_id_.find(id.value(), [fields, id](u32 candidate) { return fields[candidate].id == id; });
    return (found == detail::ProbeTable::absent) ? nullptr : &fields[found];
}

const FieldInfo* FieldIndex::find(const char* name) const noexcept {
    detail::note_reflected_lookup();
    if (type_ == nullptr) {
        return nullptr;
    }
    const FieldInfo* fields = type_->fields;
    const u32 found = by_name_.find(detail::hash_name(name), [fields, name](u32 candidate) {
        return same_string(fields[candidate].name, name);
    });
    return (found == detail::ProbeTable::absent) ? nullptr : &fields[found];
}

u32 FieldIndex::longest_probe() const noexcept {
    const u32 by_id = by_id_.longest_probe();
    const u32 by_name = by_name_.longest_probe();
    return (by_id > by_name) ? by_id : by_name;
}

}  // namespace cy::reflect
