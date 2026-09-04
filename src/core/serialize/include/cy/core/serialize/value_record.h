#pragma once
// The value record: a mapping from `FieldId` to an encoded value. Tasks 3.2.3, 3.2.6 and 3.2.9.
//
// `serialization-and-prefabs` names this structure twice and both times it is load-bearing.
//
//   * "Value-level migration": the load path is `serialized record → value record → migration chain
//     → current schema → native object`, and a migration operates on the value record "and SHALL
//     NOT require constructing an instance of an older version of the type, which no longer exists
//     in the code". That is only possible if there is a representation of a type's data that does
//     not need the type.
//   * "Unknown data is preserved": a field whose `FieldId` is not in the current schema "SHALL be
//     preserved through a load and save round trip by default, not discarded".
//
// One structure answers both, and it answers the second for free. A value record holds whatever was
// read — a `FieldId` it does not recognise is a value like any other, because the record has no
// schema to check it against. Applying the record to an object skips it; writing the record back
// writes it. Preservation is therefore not a feature with a code path that can be forgotten; it is
// what happens when nothing goes out of its way to drop data.
//
// It is also the authoring representation of a component and the payload of a prefab override, for
// the same reason: an override addresses a field on a type that may not be loaded in this build,
// and it has to survive that.
//
// **A value record is not a runtime structure.** The specification says so — "the value record
// SHALL exist only during migration and tooling, and SHALL NOT appear in runtime hot paths" — and
// this implementation makes it awkward to misuse: it allocates, its lookup is a binary search, and
// the packed form the runtime reads (cooked.h) has no record in it at all.
//
// ORDER IS BY `FieldId`, NOT BY INSERTION. Two records holding the same values are byte-identical
// whatever order they were built in, which is what makes the text form's "one changed property, one
// changed line" true after a load-and-save round trip rather than only on the first write.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/field_index.h>
#include <cy/core/reflect/ids.h>
#include <cy/core/reflect/type_info.h>
#include <cy/core/serialize/classification.h>
#include <cy/core/serialize/traversal.h>
#include <cy/core/serialize/wire.h>

namespace cy::serialize {

/// Where one field's bytes are, and how to read them. The bytes themselves live in the record's
/// pool, so this stays a small trivially copyable value that an `Array` can move with a memcpy.
struct FieldValue {
    reflect::FieldId id;
    WireType wire = WireType::Bytes;
    u32 offset = 0;  ///< Into the record's byte pool.
    u32 size = 0;
};

/// One type's data, addressed by identifier.
class ValueRecord {
public:
    explicit ValueRecord(Allocator& allocator = current_allocator()) noexcept
        : fields_(allocator), pool_(allocator) {}

    // Move-only, like every owning container in this engine: a copy is `clone_into`, which can fail
    // and says so at the call site.
    ValueRecord(const ValueRecord&) = delete;
    ValueRecord& operator=(const ValueRecord&) = delete;
    ValueRecord(ValueRecord&&) noexcept = default;
    ValueRecord& operator=(ValueRecord&&) noexcept = default;
    ~ValueRecord() = default;

    /// The type the record describes, and the schema version its data was written against.
    ///
    /// The version is the record's, not the type's: it is what the migration chain starts from, and
    /// it stays at the written value until a migration advances it.
    [[nodiscard]] reflect::TypeId type() const noexcept { return type_; }
    [[nodiscard]] u16 schema_version() const noexcept { return schema_version_; }
    void set_type(reflect::TypeId type) noexcept { type_ = type; }
    void set_schema_version(u16 version) noexcept { schema_version_ = version; }

    /// Store a field's bytes exactly as given. The bytes are copied into the record's pool.
    ///
    /// Replaces any existing value for the identifier, so applying an override on top of a base is
    /// a sequence of `set` calls in precedence order and nothing else.
    [[nodiscard]] Status set(reflect::FieldId id, WireType wire, const void* bytes,
                             u32 size) noexcept;

    /// Store a scalar held in the host's representation, encoding it to the wire's byte order.
    [[nodiscard]] Status set_scalar(reflect::FieldId id, WireType wire, const void* value,
                                    u32 size) noexcept;

    /// Store a reference to an entity in the same document, by its author-time local id.
    [[nodiscard]] Status set_local_reference(reflect::FieldId id, u32 local) noexcept;

    /// Store a reference into another document: the document's asset id halves, then the local id.
    [[nodiscard]] Status set_external_reference(reflect::FieldId id, u64 asset_high, u64 asset_low,
                                                u32 local) noexcept;

    /// Read a local reference back. Fails when the field is absent or is not a `LocalRef`.
    [[nodiscard]] Expected<u32, Error> local_reference(reflect::FieldId id) const noexcept;

    [[nodiscard]] const FieldValue* find(reflect::FieldId id) const noexcept;
    [[nodiscard]] bool contains(reflect::FieldId id) const noexcept { return find(id) != nullptr; }

    /// The bytes of a field this record holds. Empty when the descriptor is not this record's.
    [[nodiscard]] Span<const u8> bytes(const FieldValue& value) const noexcept;
    [[nodiscard]] Span<const u8> bytes(reflect::FieldId id) const noexcept;

    /// Remove a field. True when there was one. What "discard the override" does, and what a
    /// migration that deletes a field does.
    bool remove(reflect::FieldId id) noexcept;

    /// Move a field's data to a different identifier, keeping its bytes and wire type.
    ///
    /// This is what a migration that changes a field's identity does, and it is the same operation
    /// an override-target migration performs — which is why it is one function rather than two.
    /// Fails when `to` is already present, because silently merging two fields into one would lose
    /// whichever lost the race.
    [[nodiscard]] Status retarget(reflect::FieldId from, reflect::FieldId to) noexcept;

    /// Every field, in ascending identifier order.
    [[nodiscard]] Span<const FieldValue> fields() const noexcept { return fields_.span(); }
    [[nodiscard]] usize size() const noexcept { return fields_.size(); }
    [[nodiscard]] bool empty() const noexcept { return fields_.empty(); }

    void clear() noexcept;

    /// A deep copy. Explicit and fallible, because it allocates.
    [[nodiscard]] Status clone_into(ValueRecord& out) const noexcept;

    /// Copy every field of `other` over this record's, leaving fields `other` does not carry alone.
    ///
    /// The override composition operator: base, then variant, then instance, each layered on with
    /// this call. Fields the later layer does not mention keep the earlier layer's value, which is
    /// what "an instance stores only its differences from the source" means when it is applied.
    [[nodiscard]] Status overlay(const ValueRecord& other) noexcept;

    [[nodiscard]] Allocator& allocator() const noexcept { return fields_.allocator(); }

private:
    /// The index at which `id` sits or would sit. Binary search over the sorted descriptors.
    [[nodiscard]] usize lower_bound(reflect::FieldId id) const noexcept;

    reflect::TypeId type_;
    u16 schema_version_ = 0;
    Array<FieldValue> fields_;
    /// Field bytes, appended. A field written twice at different widths leaves its first bytes
    /// stranded here; a value record is built once and thrown away, and compacting it would cost
    /// more than the bytes are worth.
    Array<u8> pool_;
};

/// Fill `out` from `object` by walking `type`, honouring the classification table for `purpose`.
///
/// This is `visit_object()` with a record-building visitor, and it is the only route from a native
/// object into a record — so the fields a record contains are exactly the fields the tagged and
/// text writers would have written, and a test of one is a test of all three.
[[nodiscard]] Status record_from_object(const reflect::TypeInfo& type, const void* object,
                                        Purpose purpose, ValueRecord& out) noexcept;

/// Write `record`'s fields into `object`, resolving identifiers through `fields`.
///
/// Three rules, each from the specification:
///
///   * a field the record carries that the type no longer has is **skipped**, not an error — that
///     is a removed field, and the manifest's tombstone is what makes skipping it safe;
///   * a field the type has that the record does not carry is **left as the caller initialised
///     it**, which is how a field added after the data was written takes its default;
///   * a field whose recorded width does not match the type's is an **error**, not a
///     reinterpretation of bytes that mean something else.
///
/// `applied` receives how many fields were written, and `skipped` how many were preserved-unknown.
/// Both are reportable, because "preservation SHALL be bounded and reportable".
[[nodiscard]] Status record_to_object(const ValueRecord& record, const reflect::FieldIndex& fields,
                                      void* object, u32* applied = nullptr,
                                      u32* skipped = nullptr) noexcept;

}  // namespace cy::serialize
