// Round-tripping reflected data by identifier. Task 1.2.5.
//
// This is the smallest consumer of the identity model that can prove the model works, and it is the
// one the golden tests are written against. Three properties are the point:
//
//   * A record addresses fields by **FieldId**. A byte offset never appears in it, because layout
//     is a compiler artefact — so inserting a field, reordering fields, or changing a field's type
//     upstream does not invalidate what is already written.
//   * A **name** never appears in it either. Renaming a field is a manifest edit and nothing else.
//   * A record whose TypeId is not registered is **preserved verbatim**, so a host that does not
//     know a type does not destroy it by re-saving. `core-type-system` requires exactly that.
//
// The encoding is little-endian and explicit, byte by byte, so a golden committed on one machine
// reads identically on another. It is not the shipping serializer — `serialization-and-prefabs`
// owns that, with its text and binary forms, at a later milestone. It is the round-trip that makes
// the identity claim testable now, and it is control-plane code: the writer walks field descriptors
// and the reader looks fields up by identifier, which is precisely the work a per-frame path is
// forbidden to do.
//
//   record  := u32 type_id | u32 payload_size | u16 field_count | u16 reserved | payload
//   payload := field*
//   field   := u32 field_id | u8 kind | u8 reserved | u16 size | u8 value[size]

#ifndef CY_CORE_REFLECT_SERIALIZE_H
#define CY_CORE_REFLECT_SERIALIZE_H

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/reflect/field_index.h>
#include <cy/core/reflect/ids.h>
#include <cy/core/reflect/type_info.h>

namespace cy::reflect {

/// Bytes to append to. Deliberately minimal: the allocator interface and the engine's containers
/// are the memory module's work at M1 (`core-memory-and-containers`), and this module must not
/// invent a second one to be replaced later. Growth doubles; a failed allocation is reported
/// through Status rather than thrown, because nothing in this engine throws.
class ByteBuffer {
public:
    ByteBuffer() noexcept = default;
    ~ByteBuffer();

    ByteBuffer(const ByteBuffer&) = delete;
    ByteBuffer& operator=(const ByteBuffer&) = delete;

    Status append(const void* bytes, usize count);

    [[nodiscard]] const u8* data() const noexcept { return data_; }
    [[nodiscard]] usize size() const noexcept { return size_; }
    void clear() noexcept { size_ = 0; }

private:
    Status reserve(usize wanted);

    u8* data_ = nullptr;
    usize size_ = 0;
    usize capacity_ = 0;
};

/// The fixed part of a record, readable without knowing the type.
struct RecordHeader {
    TypeId type;
    u32 payload_size = 0;
    u16 field_count = 0;

    /// Total bytes of the record, header included. This is what a reader advances by, and what lets
    /// an unknown record be copied forward without being understood.
    [[nodiscard]] constexpr usize total_size() const noexcept { return header_size + payload_size; }

    static constexpr usize header_size = 12;
};

/// Append `object`, described by `type`, as one record.
///
/// Fields declared `Transient` are skipped: `core-type-system` requires they be excluded from
/// serialization and from replication, and skipping them at the writer means no reader has to know.
Status write_record(const TypeInfo& type, const void* object, ByteBuffer& out);

/// Read the header of the record at the front of `data`, without needing its type.
Expected<RecordHeader, Error> peek_record(const u8* data, usize size);

/// Apply the record at the front of `data` to `object`, resolving fields through `fields`.
///
/// **This is the overload a loader uses.** Its cost is linear in the record: one hash probe per
/// field the record carries, and no scan of the type's fields at all. A scene load holds one
/// FieldIndex per type — `TypeRegistry::fields()` already built it — and decodes every record of
/// that type through this call.
///
/// A field the record carries that the type no longer has is skipped — that is a removed field, and
/// the manifest's tombstone is what makes skipping it safe. A field the type has that the record
/// does not carry is left at whatever the caller initialised it to, which is how a field added
/// after the data was written gets its default. A field whose recorded kind or width no longer
/// matches the type's is an error rather than a reinterpretation.
Status read_record(const FieldIndex& fields, const u8* data, usize size, void* object);

/// The same, for a caller that has a TypeInfo and one record.
///
/// It builds a FieldIndex, decodes through it, and throws the index away: linear in the type's
/// field count plus linear in the record, rather than the product of the two that M1's
/// find_field-per-field loop cost. **Decoding many records of one type through this overload pays
/// that build every time** — hold a FieldIndex, or ask the registry for the one it built.
Status read_record(const TypeInfo& type, const u8* data, usize size, void* object);

/// Copy one record forward without understanding it. This is what a host does with a record whose
/// TypeId the registry does not know, so that re-saving preserves it rather than dropping it.
Status write_opaque(const u8* data, usize size, ByteBuffer& out);

}  // namespace cy::reflect

#endif  // CY_CORE_REFLECT_SERIALIZE_H
