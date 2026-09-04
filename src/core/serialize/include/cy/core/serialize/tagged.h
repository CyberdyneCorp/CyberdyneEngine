#pragma once
// The tagged form: chunked, versioned, skip-unknown, bounds-checked. Tasks 3.2.2 and 3.2.3.
//
// `serialization-and-prefabs` — "Two serialization modes" — gives this half of the pair its job:
// authoring data, saves, prefab overrides, replays and editor round trips. Its guarantee is
// evolution. It skips fields it does not know, preserves them, migrates what it does know, and
// round-trips. Its cost is a tag and a length per field, which is exactly why the other half
// (cooked.h) exists and why field-tagging a million transforms is the thing the specification rules
// out by name.
//
//   stream  := header | chunk*
//   header  := u32 magic 'CYTG' | u16 format_version | u16 flags | u32 chunk_count
//   chunk   := u32 tag | u32 payload_size | u8 payload[payload_size]
//   record  := u32 type_id | u16 schema_version | u16 field_count | u32 payload_size | field*
//   field   := u32 field_id | u8 wire_type | u8 reserved | u16 size | u8 value[size]
//
// Every integer is little-endian and written byte by byte (wire.h), so a file written on one
// machine reads identically on another.
//
// FOUR PROPERTIES, AND WHERE EACH ONE LIVES.
//
//   versioned    the header carries the format version, and every record carries the *schema*
//                version of the type it holds. The two are different numbers answering different
//                questions — "can this reader parse the bytes" and "does this reader understand
//                what they mean" — and conflating them is how a format ends up unable to add a
//                field without a new parser.
//   chunked      a chunk has a tag and a length, so a reader steps over a chunk it does not know
//                and a writer adds a new kind of content without changing the readers. This is the
//                mechanism a scene uses to carry its entity table beside its unknown-plugin data.
//   skip-unknown a record carries its payload size, so a reader steps over a whole record of a type
//                it does not know; a field carries its size, so it steps over one field.
//   streamable   both directions are forward-only over a cursor. `end_stream()` patches the chunk
//                count, which is the one backwards write and is documented at `ByteWriter`.
//
// UNKNOWN DATA IS PRESERVED BY CONSTRUCTION, NOT BY A CODE PATH. `read_record()` reads into a
// `ValueRecord`, which has no schema to check anything against — so a field this build has never
// heard of is a field like any other, and `write_record()` writes it back byte for byte. There is
// no branch that could be forgotten and no flag that could default the wrong way, which is what
// "so an editor without a plugin does not silently strip that plugin's data from every file it
// touches" needs in order to be true of every file rather than of the ones somebody remembered.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/serialize/value_record.h>
#include <cy/core/serialize/wire.h>

namespace cy::serialize {

/// 'C','Y','T','G' read in file order, which is the little-endian word below.
inline constexpr u32 kTaggedMagic = 0x4754'5943U;

/// The **format** version: what a parser must understand to read the bytes at all. Distinct from a
/// record's schema version, which is what a consumer must understand to read their meaning.
inline constexpr u16 kTaggedFormatVersion = 1;

/// Four characters as one word, in the order they appear in the file. The spelling a chunk tag is
/// written with, so that a tag in the source reads the same as a tag in a hex dump.
[[nodiscard]] constexpr u32 chunk_tag(char a, char b, char c, char d) noexcept {
    return static_cast<u32>(static_cast<u8>(a)) | (static_cast<u32>(static_cast<u8>(b)) << 8U) |
           (static_cast<u32>(static_cast<u8>(c)) << 16U) |
           (static_cast<u32>(static_cast<u8>(d)) << 24U);
}

/// One chunk as a reader sees it: its tag, and a view of its payload within the reader's buffer.
struct TaggedChunk {
    u32 tag = 0;
    Span<const u8> payload;
};

/// Assembles a tagged stream into a caller-owned byte array.
class TaggedWriter {
public:
    explicit TaggedWriter(Array<u8>& out) noexcept : out_(&out), writer_(out) {}

    /// Write the stream header. Must be the first call.
    [[nodiscard]] Status begin_stream() noexcept;

    /// Open a chunk. Its length is written as a placeholder and patched by `end_chunk()`.
    [[nodiscard]] Status begin_chunk(u32 tag) noexcept;
    [[nodiscard]] Status end_chunk() noexcept;

    /// Append one record. Fields go out in ascending identifier order, which is the order a
    /// `ValueRecord` holds them in — so two writes of equal data produce equal bytes.
    [[nodiscard]] Status write_record(const ValueRecord& record) noexcept;

    /// Build a record from a native object and append it, in one call.
    [[nodiscard]] Status write_object(const reflect::TypeInfo& type, const void* object,
                                      Purpose purpose, u16 schema_version) noexcept;

    /// Patch the chunk count and finish. Reading a stream that was never ended is an error rather
    /// than a shorter stream, because a truncated write is the failure this catches.
    [[nodiscard]] Status end_stream() noexcept;

private:
    Array<u8>* out_;
    ByteWriter writer_;
    usize chunk_count_offset_ = 0;
    usize chunk_size_offset_ = 0;
    u32 chunk_count_ = 0;
    bool in_chunk_ = false;
    bool began_ = false;
};

/// Reads a tagged stream. Holds no copy of it: every span it returns addresses the caller's bytes.
class TaggedReader {
public:
    TaggedReader(const u8* data, usize size) noexcept : reader_(data, size) {}

    /// Validate the header. Fails on a wrong magic and on a format version this build cannot parse
    /// — which is a diagnostic, not a reinterpretation.
    [[nodiscard]] Status read_header() noexcept;

    [[nodiscard]] u32 chunk_count() const noexcept { return chunk_count_; }

    /// The next chunk, or `NotFound` at the end of the stream.
    [[nodiscard]] Expected<TaggedChunk, Error> next_chunk() noexcept;

private:
    ByteReader reader_;
    u32 chunk_count_ = 0;
    u32 chunks_read_ = 0;
    bool header_read_ = false;
};

/// The fixed part of a record, readable without knowing the type. What lets an unknown record be
/// stepped over exactly.
struct TaggedRecordHeader {
    reflect::TypeId type;
    u16 schema_version = 0;
    u16 field_count = 0;
    u32 payload_size = 0;

    static constexpr usize kHeaderSize = 12;
};

/// Read one record's header, leaving the cursor at its payload.
[[nodiscard]] Expected<TaggedRecordHeader, Error> read_record_header(ByteReader& reader) noexcept;

/// Read one whole record into `out`, replacing whatever it held.
///
/// Every field is kept, including fields whose identifiers this build does not recognise: that is
/// the preservation guarantee, and it is why this reads into a record rather than into an object.
[[nodiscard]] Status read_record(ByteReader& reader, ValueRecord& out) noexcept;

/// Read the records of one chunk payload into `out`, appending.
[[nodiscard]] Status read_records(Span<const u8> payload, Array<ValueRecord>& out) noexcept;

}  // namespace cy::serialize
