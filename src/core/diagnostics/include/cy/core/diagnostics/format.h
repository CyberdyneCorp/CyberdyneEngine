#pragma once
// The capture artefact's wire format, in one place, because two readers depend on it: the writer in
// this module and tools/trace/trace_inspect.py. A change here is a change to both.
//
// `diagnostics-profiling-and-crash` — "Capture artefacts": a chunked, streamable artefact openable
// without the game runtime, carrying its own metadata for identifier resolution and its build
// identity, loadable partially so a long capture opens without reading all of it. The chunk index
// at the tail is what makes the last property true.
//
// Every structure is byte-packed by construction: the fields are ordered so that no compiler
// inserts padding, and each size is asserted below. Values are little-endian; the engine has no
// big-endian target and the reader states the assumption rather than paying for it.
//
// M0 writes `Compression::None`. The chunk header carries the compressed and uncompressed lengths
// already, so turning on zstd — the dependency exists for exactly this, see deps/manifest.toml — is
// a change to the writer and the reader, not to the format.

#include <cy/core/diagnostics/prelude.h>

namespace cy::diag::format {

inline constexpr u32 kFormatVersion = 1;
inline constexpr char kMagic[8] = {'C', 'Y', 'T', 'R', 'A', 'C', 'E', '\0'};

enum class Compression : u8 {
    None = 0,
    Zstd = 1,  // reserved; not written by M0
};

/// A four-character chunk tag, little-endian, so that `xxd` on a capture reads the tags out.
constexpr u32 chunk_tag(char a, char b, char c, char d) noexcept {
    return static_cast<u32>(static_cast<u8>(a)) | (static_cast<u32>(static_cast<u8>(b)) << 8) |
           (static_cast<u32>(static_cast<u8>(c)) << 16) |
           (static_cast<u32>(static_cast<u8>(d)) << 24);
}

inline constexpr u32 kChunkMeta = chunk_tag('M', 'E', 'T', 'A');  // identifier tables and identity
inline constexpr u32 kChunkEvents = chunk_tag('E', 'V', 'T', 'S');  // one thread's records
inline constexpr u32 kChunkLoss = chunk_tag('L', 'O', 'S', 'S');    // what was dropped, and why
inline constexpr u32 kChunkIndex = chunk_tag('E', 'N', 'D', 'X');   // the chunk index, written last

struct FileHeader {
    char magic[8];
    u32 format_version;
    u32 header_bytes;
    u64 trace_id;
    u64 wall_ns;       // unix epoch nanoseconds when the trace opened
    u64 monotonic_ns;  // the monotonic clock at the same instant; event stamps share this clock
    u32 process_id;
    u8 max_classification;  // the ExportPolicy ceiling this artefact was written under
    u8 compression;
    u16 flags;
    u64 reserved0;
    u64 reserved1;
};

struct ChunkHeader {
    u32 tag;
    u32 flags;
    u64 payload_bytes;       // bytes on disk
    u64 uncompressed_bytes;  // equal to payload_bytes while Compression::None
};

/// A record's first eight bytes, in the per-thread ring and on disk unchanged. `size` counts the
/// header and is always a multiple of eight, so a reader advances without alignment arithmetic.
struct RecordHeader {
    u16 size;
    u8 kind;
    u8 channel;
    u32 name;
};

/// The fixed part of every record except Padding. `a` and `b` are the event's machine-word payload:
/// a counter's value, a flow's correlation id, an allocation's size, a tick's state hash.
struct RecordBody {
    u64 timestamp_ns;
    u32 category;
    u16 field_count;
    u16 text_bytes;
    u64 a;
    u64 b;
};

/// One structured field. A string field's bytes live in the record's text area at `text_offset`,
/// `bits` long; every other type carries its value in `bits`, a double bit-cast into it.
struct FieldRecord {
    u32 field;
    u16 flags;
    u16 text_offset;
    u64 bits;
};

inline constexpr u16 kFieldRedacted = 1u << 0;

inline constexpr u32 kRecordAlignment = 8;
inline constexpr u32 kRecordFixedBytes =
    static_cast<u32>(sizeof(RecordHeader) + sizeof(RecordBody));

static_assert(sizeof(FileHeader) == 64, "the file header is a fixed 64 bytes");
static_assert(sizeof(ChunkHeader) == 24, "the chunk header is a fixed 24 bytes");
static_assert(sizeof(RecordHeader) == 8, "records are eight-byte aligned and so is their header");
static_assert(sizeof(RecordBody) == 32, "the record body is a fixed 32 bytes");
static_assert(sizeof(FieldRecord) == 16, "a field record is a fixed 16 bytes");

/// Why a record or a field did not reach the artefact. Reported in the LOSS chunk and as Loss
/// events on the timeline, because silent loss is a forbidden pattern.
enum class LossReason : u8 {
    BufferPressure = 0,  // the per-thread buffer was above this channel's admission threshold
    RecordTooLarge = 1,  // one record exceeded the whole buffer; a producer bug, counted not hidden
    UnclassifiedField = 2,  // the field id carried no registered classification, so it was redacted
    RegistryFull = 3,       // the metadata table is fixed-capacity and was full at registration
    PolicyRedaction = 4,    // the field's classification is above the artefact's declared ceiling
};

}  // namespace cy::diag::format
