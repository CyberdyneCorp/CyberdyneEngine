// A type's fields, resolved once and thereafter looked up in constant time. M2 task 1.1.
//
// M1 shipped `TypeInfo::find_field()`, a linear scan, and `read_record()` called it once per field
// per record. With two reflected types of five fields nobody noticed; the cost is the *product* of
// the record's field count and the type's, and M2 reflects hundreds of types and loads whole scenes
// through that one function. The spec delta states the contract plainly:
//
//     Type and field lookup SHALL NOT be linear in the number of registered types or fields. A
//     record decode SHALL be linear in the size of the record, not in the product of its field
//     count and the type's field count.
//
// This is the half of it that is about fields. A FieldIndex is built once from a TypeInfo — one
// pass over its descriptors, one allocation — and every lookup afterwards is a hash probe whose
// length does not grow with the field count. It is the same shape as TypedAccessor one level up:
// pay the reflected cost at setup, and hold something afterwards that cannot pay it again.
//
// WHY THIS IS NOT A MEMBER OF TypeInfo. A TypeInfo is constexpr data in a generated translation
// unit — it is emitted by tools/gen/, it is immutable, and it allocates nothing. A hash table
// cannot live inside one without either the generator emitting it (a table that would then have to
// be regenerated for a change no annotated header made) or the descriptor becoming mutable at run
// time (which would make the metadata a shared mutable object every thread touches). Building the
// index beside the descriptor keeps the descriptor exactly what it was.
//
// WHO SHOULD HOLD ONE. Anything that looks a field up more than once for the same type: the
// serializer, the scene loader, the property inspector, a migration. `TypeRegistry` builds one per
// registered type and hands it out through `TypeRegistry::fields()`, so the common case — a loader
// that has a TypeId and a stream of records — never builds one at all.
//
// It is control-plane code and reports itself as such: a lookup here notes itself to
// control_plane.h exactly as the linear scan it replaces did, because "not linear" is not the same
// claim as "free", and a per-frame path holding a FieldIndex is still a per-frame path doing
// reflection.

#ifndef CY_CORE_REFLECT_FIELD_INDEX_H
#define CY_CORE_REFLECT_FIELD_INDEX_H

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/reflect/ids.h>
#include <cy/core/reflect/probe_table.h>
#include <cy/core/reflect/type_info.h>

namespace cy::reflect {

class FieldIndex {
public:
    FieldIndex() noexcept = default;
    ~FieldIndex();

    // Copying would leave two owners of one slot table. Moving is fine: the tables address a heap
    // block that does not move with the object.
    FieldIndex(const FieldIndex&) = delete;
    FieldIndex& operator=(const FieldIndex&) = delete;
    FieldIndex(FieldIndex&& other) noexcept;
    FieldIndex& operator=(FieldIndex&& other) noexcept;

    /// Index `type`'s fields. One pass and one allocation, both linear in the field count.
    ///
    /// The descriptor is borrowed and must outlive the index — it is constexpr data in a generated
    /// translation unit, so in practice it outlives everything.
    ///
    /// Fails when two fields of one type claim one FieldId. That cannot happen through the
    /// generator, because the identity manifest assigns the numbers and tombstones them on removal;
    /// it is checked here because this is the first code that can see it, and a duplicate would
    /// otherwise decode one field's bytes into another's storage.
    Status build(const TypeInfo& type);

    /// The type this index was built for, or null before build().
    [[nodiscard]] const TypeInfo* type() const noexcept { return type_; }

    /// The field with this identifier, or null. Constant expected time; no scan.
    [[nodiscard]] const FieldInfo* find(FieldId id) const noexcept;

    /// The field with this name, or null. Names are metadata — for tooling and diagnostics, never
    /// for anything that persists a reference.
    [[nodiscard]] const FieldInfo* find(const char* name) const noexcept;

    /// The longest probe chain either table holds, measured as it was built.
    ///
    /// A diagnostic, and the assertion the scaling test is written against: a linear scan's worst
    /// case grows with the field count and this number does not, which is a claim a test can make
    /// deterministically on any machine rather than with a stopwatch.
    [[nodiscard]] u32 longest_probe() const noexcept;

private:
    void release() noexcept;

    const TypeInfo* type_ = nullptr;

    /// One block, two tables: `slot_count_` slots keyed by FieldId, then as many keyed by name.
    /// One allocation rather than two, because they are always built and released together.
    u32* slots_ = nullptr;
    u32 slot_count_ = 0;

    detail::ProbeTable by_id_;
    detail::ProbeTable by_name_;
};

/// Resolve a field to a typed accessor through an index rather than a scan.
///
/// The same contract as the TypeInfo overload in type_info.h — one reflected lookup at setup, and
/// an accessor afterwards that holds a byte offset and cannot look anything up.
template <typename T>
[[nodiscard]] Expected<TypedAccessor<T>, Error> resolve_field(const FieldIndex& fields,
                                                              FieldId id) {
    const FieldInfo* field = fields.find(id);
    if (field == nullptr) {
        return fail(ErrorCode::NotFound, "no field with that FieldId on this type");
    }
    if (field->kind != FieldKindOf<T>::value || field->size != sizeof(T)) {
        return fail(ErrorCode::InvalidArgument, "field does not hold the requested type");
    }
    return TypedAccessor<T>(field->offset);
}

}  // namespace cy::reflect

#endif  // CY_CORE_REFLECT_FIELD_INDEX_H
