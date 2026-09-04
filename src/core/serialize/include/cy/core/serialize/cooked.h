#pragma once
// The cooked form: packed, untagged, and copied rather than parsed. Task 3.2.2.
//
// `serialization-and-prefabs` — "Two serialization modes" — gives this half its job and its
// prohibition. Its job is runtime assets, ECS cell and chunk data, and GPU-ready structures. Its
// prohibition is per-field tags: "Field-tagging bulk runtime data — a million transforms in a world
// cell — SHALL NOT occur." So a block here is column bytes and a table saying where they go, and
// nothing per field and nothing per row.
//
// It has no evolution mechanism at all, and that is deliberate: the cooker and the runtime share
// one build. What replaces evolution is a **build schema identity** recorded in the stream, so that
// cooked data produced against a different schema is a diagnostic rather than a reinterpretation of
// packed bytes. `BuildSchemaDigest` below is what computes it, over exactly the things a packed
// layout depends on — a type's identity, its schema version, its size and alignment, and every
// field's identifier, kind, offset and width. Change any of those and the number changes.
//
// --- WHY A BLOCK CARRIES A REFERENCE-SITE TABLE
// ---------------------------------------------------
//
// This is the M2 spike's finding, and it is a requirement rather than an optimisation.
//
// A cooked block is chunk-shaped and copies into a chunk as whole-column memcpys — but a copy alone
// does not activate it. Two things in the destination cannot be known at cook time: the key column,
// which holds live `Entity` values the cooker has never seen, and every entity-reference slot,
// which holds a cook-time local index rather than an id. Both are rewritten after the copy. The
// spike measured that at 21.4 % of the payload and a quarter of activation time, which is
// affordable — and measured the alternative, asking a component registry per row which columns hold
// references, at 4.7 to 5.2 times the cost of a table lookup.
//
// So the cooker emits the sites: the column, and the byte offset of the entity-typed field within
// that column's element. Activation is then a strided pass over known columns. Without the table
// the fixup becomes the reflection walk the archetype storage exists to avoid, and *that* is the
// outcome that would collapse the storage argument — not the fixup itself.
//
//   stream := u32 magic 'CYCK' | u16 format_version | u16 flags | u64 build_schema | u32
//   block_count
//             block*
//   block  := u32 row_count | u32 column_count | u32 site_count | u32 payload_size
//             column* | site* | u8 payload[payload_size]
//   column := u32 type_id | u32 element_size
//   site   := u16 column_index | u16 reserved | u32 byte_offset
//
// The payload holds each column's rows contiguously, in column order: column `c` occupies
// `row_count * element_size(c)` bytes, and the columns follow one another with no padding, because
// the destination decides alignment and the file is copied column by column rather than whole.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/ids.h>
#include <cy/core/reflect/type_info.h>

namespace cy::serialize {

/// 'C','Y','C','K' in file order.
inline constexpr u32 kCookedMagic = 0x4B43'5943U;
inline constexpr u16 kCookedFormatVersion = 1;

/// Accumulates the build schema identity over every type a cooked artefact depends on.
///
/// Order-independent by construction: each type's contribution is mixed in with an operation that
/// commutes, so two cookers that visit the same types in different orders agree. That matters
/// because the runtime computes the same number by walking its own registry, which is in
/// registration order, and the cooker walks the types a scene happens to use.
class BuildSchemaDigest {
public:
    /// Mix in one type: its identity, its schema version, its layout, and every field's identity,
    /// kind, offset and width.
    void add_type(const reflect::TypeInfo& type, u16 schema_version) noexcept;

    [[nodiscard]] u64 value() const noexcept { return value_; }
    [[nodiscard]] u32 type_count() const noexcept { return type_count_; }

private:
    u64 value_ = 0;
    u32 type_count_ = 0;
};

/// One column of a cooked block: what type it holds and how wide one element is.
struct CookedColumn {
    reflect::TypeId type;
    u32 element_size = 0;
};

/// The width of one entity reference in a cooked column.
///
/// Layer 0 cannot name `cy::ecs::Entity` — the ECS is layer 1 and an upward include fails the build
/// — so the width is a constant here and `src/scene/serialization/` static_asserts it against the
/// real type. That is the seam, stated rather than assumed: if an entity ever stops being eight
/// bytes, the assertion at layer 4 fails at compile time rather than the reference fixup writing
/// four bytes into an eight-byte slot.
inline constexpr u32 kEntityReferenceBytes = 8;

/// Where one entity reference sits: which column, and the byte offset within one element of it.
struct ReferenceSite {
    u16 column = 0;
    u32 offset = 0;
};

/// One archetype's worth of prepared rows.
///
/// The columns and sites are owned; the payload is a view, because a reader hands back a view of
/// the caller's bytes and a writer is handed one. Nothing here copies a payload.
class CookedBlock {
public:
    explicit CookedBlock(Allocator& allocator = current_allocator()) noexcept
        : columns_(allocator), sites_(allocator) {}

    [[nodiscard]] Status add_column(reflect::TypeId type, u32 element_size) noexcept;
    [[nodiscard]] Status add_reference_site(u16 column, u32 offset) noexcept;

    void set_row_count(u32 count) noexcept { row_count_ = count; }
    void set_payload(Span<const u8> payload) noexcept { payload_ = payload; }

    [[nodiscard]] u32 row_count() const noexcept { return row_count_; }
    [[nodiscard]] Span<const CookedColumn> columns() const noexcept { return columns_.span(); }
    [[nodiscard]] Span<const ReferenceSite> reference_sites() const noexcept {
        return sites_.span();
    }
    [[nodiscard]] Span<const u8> payload() const noexcept { return payload_; }

    /// The bytes of one column: `row_count * element_size` of them, contiguous.
    [[nodiscard]] Expected<Span<const u8>, Error> column_bytes(usize index) const noexcept;

    /// Total payload bytes the declared columns account for. What a reader checks the file against.
    [[nodiscard]] usize expected_payload_size() const noexcept;

    void clear() noexcept;

private:
    Array<CookedColumn> columns_;
    Array<ReferenceSite> sites_;
    Span<const u8> payload_;
    u32 row_count_ = 0;
};

/// Assembles a cooked stream.
class CookedWriter {
public:
    explicit CookedWriter(Array<u8>& out) noexcept : out_(&out) {}

    [[nodiscard]] Status begin_stream(u64 build_schema) noexcept;
    [[nodiscard]] Status write_block(const CookedBlock& block) noexcept;
    [[nodiscard]] Status end_stream() noexcept;

private:
    Array<u8>* out_;
    usize block_count_offset_ = 0;
    u32 block_count_ = 0;
    bool began_ = false;
};

/// Reads a cooked stream, checking the build schema identity before anything else.
class CookedReader {
public:
    CookedReader(const u8* data, usize size) noexcept : data_(data), size_(size) {}

    /// Validate magic, format version, and the build schema identity.
    ///
    /// A schema mismatch fails here — before any block is read and before any byte is copied — with
    /// `ErrorCode::Unsupported`. That is the specification's "cooked data mismatch is fatal"
    /// scenario, and failing at the header rather than at the first bad row is what makes the
    /// diagnostic name the cause rather than a symptom.
    [[nodiscard]] Status read_header(u64 expected_build_schema) noexcept;

    [[nodiscard]] u64 build_schema() const noexcept { return build_schema_; }
    [[nodiscard]] u32 block_count() const noexcept { return block_count_; }

    /// Fill `out` with the next block, or `NotFound` at the end of the stream. The block's payload
    /// views this reader's buffer.
    [[nodiscard]] Status next_block(CookedBlock& out) noexcept;

private:
    const u8* data_ = nullptr;
    usize size_ = 0;
    usize offset_ = 0;
    u64 build_schema_ = 0;
    u32 block_count_ = 0;
    u32 blocks_read_ = 0;
    bool header_read_ = false;
};

}  // namespace cy::serialize
