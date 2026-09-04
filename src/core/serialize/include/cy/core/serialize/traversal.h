#pragma once
// One reflection-driven traversal; the writers are what differ. Task 3.2.2, design.md §6.
//
// `serialization-and-prefabs` requires two serialization modes and requires both to be driven by
// the type registry — "a type is serializable because it is reflected, not because it implements a
// serializer". design.md §6 turns that into a structural decision: **one traversal, two writers.**
//
// The reason to write it down as an interface rather than as an intention is that the two forms
// disagree about everything except which fields exist. The tagged form writes an identifier and a
// length per field; the text form writes a line per field; the cooked form writes no field markers
// at all. If each walked the descriptors itself, then `Transient` would be honoured in two places
// and forgotten in the third, and the classification table would have three readers that drift.
// Here the walk exists once, in `visit_object()`, and a writer is a `FieldVisitor`.
//
// WHAT THE TRAVERSAL DECIDES, AND WHAT IT DOES NOT.
//
//   it decides   which fields are visited: `Transient` and the classification table (see
//                classification.h), in that order, from one place.
//   it decides   the order: declaration order, which is what the generated descriptor array holds,
//                which is stable across builds of the same tree. Determinism of the text form
//                depends on this, so it is a property rather than an accident.
//   it does not  decide the encoding. A visitor is handed the descriptor and a pointer to the
//                field's bytes, and does whatever its form does with them.
//
// A visitor is a virtual interface rather than a template parameter on purpose. The engine compiles
// with -fno-rtti, which forbids `dynamic_cast` and `typeid` and has nothing to say about virtual
// dispatch; and this is control-plane code, where one indirect call per field is not measurable
// against the reflected lookup that found the field. What a template would buy is inlining on a
// path that already costs a hash probe, at the price of every writer's implementation moving into a
// header.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/reflect/type_info.h>
#include <cy/core/serialize/classification.h>

namespace cy::serialize {

/// What a traversal reports. One call per object, one per field it visits, one to close.
///
/// Every call returns `Status`: a writer whose buffer could not grow stops the traversal at the
/// field that failed rather than finishing and reporting a truncated result as a success.
class FieldVisitor {
public:
    FieldVisitor() noexcept = default;
    virtual ~FieldVisitor() noexcept = default;

    FieldVisitor(const FieldVisitor&) = delete;
    FieldVisitor& operator=(const FieldVisitor&) = delete;
    FieldVisitor(FieldVisitor&&) = delete;
    FieldVisitor& operator=(FieldVisitor&&) = delete;

    /// The object is about to be walked. `field_count` is how many fields will be visited — already
    /// filtered — so a writer that needs the count in a header does not have to walk twice.
    virtual Status begin_object(const reflect::TypeInfo& type, u32 field_count) noexcept = 0;

    /// One field. `value` addresses the field's bytes within the object; `field.size` is how many.
    virtual Status visit_field(const reflect::FieldInfo& field, const void* value) noexcept = 0;

    virtual Status end_object(const reflect::TypeInfo& type) noexcept = 0;
};

/// How many of a type's fields a given purpose writes. What `begin_object` is handed, and what a
/// caller sizing a buffer asks for.
[[nodiscard]] u32 written_field_count(const reflect::TypeInfo& type, Purpose purpose) noexcept;

/// Walk `object`, described by `type`, reporting to `visitor`.
///
/// This is the whole traversal. Every form in this module — tagged, text, cooked — is a visitor
/// passed to this function, which is what design.md §6 means and what stops the three from
/// disagreeing about which fields exist.
[[nodiscard]] Status visit_object(const reflect::TypeInfo& type, const void* object,
                                  Purpose purpose, FieldVisitor& visitor) noexcept;

}  // namespace cy::serialize
