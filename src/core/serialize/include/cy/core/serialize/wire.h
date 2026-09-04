#pragma once
// The byte level of the tagged form: what a value looks like on the wire, and the two cursors that
// put one there and take one back. Task 3.2.3.
//
// `serialization-and-prefabs` requires tagged data to be "versioned, chunked, of defined
// endianness, bounds-checkable, skip-unknown-capable, and streamable". Four of those six are
// decided here:
//
//   defined endianness   every integer is written byte by byte, least significant first, by shifts
//                        rather than by a memcpy of the host's representation. A golden file
//                        written on one machine therefore reads identically on another, and a
//                        big-endian port is a rebuild rather than a format change.
//   bounds-checkable     `ByteReader` carries its end and every read returns `Expected`. There is
//   no
//                        spelling of a read that does not check, because the one that skipped the
//                        check is the one that would be used in the loop.
//   skip-unknown         a value carries a `WireType` and an explicit byte length, so a reader that
//                        does not recognise a field can still step over it exactly.
//   streamable           reads and writes are forward-only over a cursor; nothing seeks backwards
//                        and nothing requires the whole stream to be resident.
//
// WHY A WIRE TYPE AND NOT JUST A LENGTH. A length alone lets a reader skip a field it does not
// know. It does not let a reader *interpret* one — and two things in this engine need exactly that.
// A migration operates on a value record whose type may no longer have the field, so it needs to
// know that four bytes are an `f32` rather than a `u32`. And an entity reference has to be
// recognisable without the schema, because remapping references at load and at cell activation is
// done by walking declared reference sites rather than by asking reflection what each field means.
// `LocalRef` and `ExternalRef` are therefore wire types, not conventions.
//
// This header knows nothing about entities, scenes or prefabs, and must not: `cy::ecs` is layer 1
// and this is layer 0. A `LocalRef` here is a 32-bit number whose meaning belongs to whoever wrote
// it; `src/scene/serialization/` is where it becomes a reference to an authored entity.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/type_info.h>

namespace cy::serialize {

/// How the bytes of one field are to be read. Persistent: these numbers are written into files, so
/// an enumerator is added at the end and never renumbered.
enum class WireType : u8 {
    Bytes = 0,  ///< An opaque run. What an unrecognised field decays to, and what a blob is.
    Bool = 1,
    I8 = 2,
    I16 = 3,
    I32 = 4,
    I64 = 5,
    U8 = 6,
    U16 = 7,
    U32 = 8,
    U64 = 9,
    F32 = 10,
    F64 = 11,
    Enum = 12,   ///< An enumerator, stored in the underlying integer's width.
    Flags = 13,  ///< A bit set, stored in the underlying integer's width.

    /// A reference to an entity within the same authoring document, as a 32-bit local id.
    LocalRef = 14,
    /// A reference into another document: a 128-bit `AssetId` followed by a 32-bit local id.
    /// Twenty bytes, written in that order.
    ExternalRef = 15,

    /// One past the last defined value. A reader rejects anything at or above it rather than
    /// guessing, because a wire type it does not know is data written by a build it is not.
    /// Numbered explicitly like every enumerator above it, so adding one is two edits and a
    /// forgotten second edit is a compile error rather than a silently shifted boundary.
    Count = 16,
};

/// The enumerator's own spelling, for a diagnostic. Never null.
const char* wire_type_name(WireType type) noexcept;

/// The fixed width of a wire type, or zero when it is variable (`Bytes`, and the three widths
/// `Enum` and `Flags` may take). Zero means "read the length that was written".
[[nodiscard]] constexpr u32 wire_type_width(WireType type) noexcept {
    switch (type) {
        case WireType::Bool:
        case WireType::I8:
        case WireType::U8:
            return 1;
        case WireType::I16:
        case WireType::U16:
            return 2;
        case WireType::I32:
        case WireType::U32:
        case WireType::F32:
        case WireType::LocalRef:
            return 4;
        case WireType::I64:
        case WireType::U64:
        case WireType::F64:
            return 8;
        case WireType::ExternalRef:
            return 20;
        default:
            return 0;
    }
}

/// True for the two reference kinds. Asked at every reference-remapping site, so it has a name.
[[nodiscard]] constexpr bool is_reference(WireType type) noexcept {
    return type == WireType::LocalRef || type == WireType::ExternalRef;
}

/// The wire type a reflected field's kind maps to.
///
/// `FieldKind::Unsupported` maps to `Bytes`, deliberately: a field the reflection module cannot
/// describe is still copied through a round trip, it simply is not interpreted. Losing it would be
/// worse than not understanding it.
[[nodiscard]] constexpr WireType wire_type_of(reflect::FieldKind kind) noexcept {
    switch (kind) {
        case reflect::FieldKind::Bool:
            return WireType::Bool;
        case reflect::FieldKind::I8:
            return WireType::I8;
        case reflect::FieldKind::I16:
            return WireType::I16;
        case reflect::FieldKind::I32:
            return WireType::I32;
        case reflect::FieldKind::I64:
            return WireType::I64;
        case reflect::FieldKind::U8:
            return WireType::U8;
        case reflect::FieldKind::U16:
            return WireType::U16;
        case reflect::FieldKind::U32:
            return WireType::U32;
        case reflect::FieldKind::U64:
            return WireType::U64;
        case reflect::FieldKind::F32:
            return WireType::F32;
        case reflect::FieldKind::F64:
            return WireType::F64;
        case reflect::FieldKind::Enum:
            return WireType::Enum;
        case reflect::FieldKind::Flags:
            return WireType::Flags;
        default:
            return WireType::Bytes;
    }
}

/// A forward-only writer over a growable byte array.
///
/// It does not own the array: a stream is assembled from several writers over one buffer — a chunk
/// header, then its payload — and an owning writer would mean either a copy per chunk or a second
/// allocator.
class ByteWriter {
public:
    explicit ByteWriter(Array<u8>& out) noexcept : out_(&out) {}

    [[nodiscard]] Status write_u8(u8 value) noexcept;
    [[nodiscard]] Status write_u16(u16 value) noexcept;
    [[nodiscard]] Status write_u32(u32 value) noexcept;
    [[nodiscard]] Status write_u64(u64 value) noexcept;
    [[nodiscard]] Status write_bytes(const void* bytes, usize count) noexcept;

    /// Write `count` bytes of a scalar of `type`, taking its bytes from `value`.
    ///
    /// The host's representation is reordered rather than written through: the bytes go out least
    /// significant first whatever order the host holds them in, so a file written on one machine
    /// reads identically on another. IEEE 754 is assumed for the float widths and asserted at
    /// compile time in the source.
    [[nodiscard]] Status write_scalar(WireType type, const void* value, u32 count) noexcept;

    [[nodiscard]] usize size() const noexcept { return out_->size(); }

    /// Overwrite four bytes already written. The one backwards operation, and it exists for exactly
    /// one reason: a chunk's length is known only after its payload has been written, and the
    /// alternative is assembling every payload in a second buffer and copying it in.
    [[nodiscard]] Status patch_u32(usize offset, u32 value) noexcept;

private:
    Array<u8>* out_;
};

/// A forward-only, bounds-checked reader.
///
/// Every read returns `Expected`, and a failed read leaves the cursor where it was — so a caller
/// that reports the error is not also obliged to restore anything.
class ByteReader {
public:
    ByteReader(const u8* data, usize size) noexcept : data_(data), size_(size) {}

    [[nodiscard]] Expected<u8, Error> read_u8() noexcept;
    [[nodiscard]] Expected<u16, Error> read_u16() noexcept;
    [[nodiscard]] Expected<u32, Error> read_u32() noexcept;
    [[nodiscard]] Expected<u64, Error> read_u64() noexcept;

    /// A view of `count` bytes at the cursor, advancing past them. The view addresses the reader's
    /// own buffer and is valid for as long as that buffer is: nothing is copied.
    [[nodiscard]] Expected<Span<const u8>, Error> read_bytes(usize count) noexcept;

    /// Step over `count` bytes without reading them. What skipping an unknown field is.
    [[nodiscard]] Status skip(usize count) noexcept;

    [[nodiscard]] usize offset() const noexcept { return offset_; }
    [[nodiscard]] usize remaining() const noexcept { return size_ - offset_; }
    [[nodiscard]] bool empty() const noexcept { return offset_ >= size_; }
    [[nodiscard]] const u8* cursor() const noexcept { return data_ + offset_; }

private:
    [[nodiscard]] Status require(usize count) const noexcept;

    const u8* data_ = nullptr;
    usize size_ = 0;
    usize offset_ = 0;
};

/// Decode `count` little-endian bytes at `bytes` into the host representation of `type`, writing it
/// to `out`. The inverse of `ByteWriter::write_scalar`, and the function every reader of a value
/// record goes through.
[[nodiscard]] Status decode_scalar(WireType type, const u8* bytes, u32 count, void* out) noexcept;

}  // namespace cy::serialize
