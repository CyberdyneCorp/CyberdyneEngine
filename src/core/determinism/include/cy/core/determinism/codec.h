#pragma once
// Generated state codecs: one compiled plan per subject per purpose. M9 task 2.4.
//
// `simulation-and-determinism` — "Generated state codecs": capturing, restoring and hashing
// authoritative state "SHALL use generated codecs produced from schema metadata, **not a reflection
// walk per field per tick**", generated "per component and per state provider for each purpose that
// needs one: rollback packing, checkpoint packing, save serialisation, and hashing", and they
// "SHALL operate on archetype storage in bulk where the layout permits, rather than per entity".
//
// ================================================================================================
// "GENERATED" MEANS COMPILED FROM THE SCHEMA ONCE, AND THE WORD IS WORTH PINNING DOWN
// ================================================================================================
//
// It does not mean a code generator emitting C++. It means the thing the requirement is actually
// about: **the hot path consults no metadata**. `compile()` reads the schema once, selects the
// fields that participate in one purpose, and lowers them into a flat array of instructions —
// offset, width, how to read it. Everything after that is a loop over that array; there is no
// `TypeInfo`, no `StateSchema` and no classification test left to perform.
//
// That is checkable rather than assertable, and `tests/test_codec.cpp` checks it the one way it can
// be checked: it compiles a codec, **destroys the schema it was compiled from**, and then packs,
// unpacks and hashes with it. A codec that had kept a pointer into the schema's arrays would read
// freed memory; one that re-walked reflection per field per tick could not run at all. The
// instructions do hold each field's `name` and `id`, which are literals and stable identifiers
// owned by whoever declared them — metadata for the divergence report, never read to decide what to
// do.
//
// ================================================================================================
// FOUR PURPOSES, AND THEY DO NOT SHARE A FIELD SET
// ================================================================================================
//
// The purpose decides which fields participate, and classification.h's `participation_of()` is the
// single table that answers it — a `Predicted` field is rolled back and checkpointed but never
// hashed, because two peers legitimately disagree about a prediction and hashing it would make
// correct prediction look like a divergence. A codec per purpose is therefore not four copies of
// one thing; it is four genuinely different field sets over the same layout.
//
// `Save` is the one that refuses something: an `InternedName` field packs as its intern index,
// which `core-type-system` says is not stable across runs. That is exactly right for rollback and
// for a checkpoint restored in the same process, and exactly wrong for a file. `compile()` refuses
// it for `Save` rather than writing a number that means nothing tomorrow; the versioned, tagged
// save encoding is `save-and-persistence`'s and this is the seam where the two part company.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/hash.h>
#include <cy/core/determinism/state_schema.h>
#include <cy/core/memory/array.h>

namespace cy::determinism {

/// What a codec is for. The four `simulation-and-determinism` names.
enum class CodecPurpose : u8 {
    /// Fold authoritative values into the state hash.
    Hash = 0,
    /// Bulk copy into the in-memory rollback ring. Current layout, no tagging.
    RollbackPack,
    /// Compact copy for a replay checkpoint. Current layout; compression is the container's.
    CheckpointPack,
    /// The versioned save encoding's field selection. **Not** the save format itself.
    Save,
};

const char* codec_purpose_name(CodecPurpose purpose) noexcept;

/// Does a field of this class take part in this purpose? `participation_of()`, seen per purpose.
[[nodiscard]] constexpr bool field_participates(SimulationClass classification,
                                                CodecPurpose purpose) noexcept {
    const Participation participation = participation_of(classification);
    switch (purpose) {
        case CodecPurpose::Hash:
            return participation.hashed;
        case CodecPurpose::RollbackPack:
            return participation.rollback;
        case CodecPurpose::CheckpointPack:
            return participation.checkpoint;
        case CodecPurpose::Save:
            return participation.saved;
    }
    return false;
}

/// One lowered field. Everything the hot path needs and nothing it has to look up.
struct CodecInstruction {
    /// The stable identity a divergence report names.
    u64 field_id = 0;
    /// Metadata for that report. A literal owned by the declarer.
    const char* name = "";
    u32 offset = 0;
    /// Bytes, derived from `kind` at compile time. `pack`/`unpack` read only this and `offset`.
    u8 width = 0;
    reflect::FieldKind kind = reflect::FieldKind::Unsupported;
    StateEncoding encoding = StateEncoding::Direct;
};

/// How much of the hierarchy a hashing codec builds.
///
/// `simulation-and-determinism` requires the hash to be narrowable to a field and separately
/// requires its frequency to be configurable; the detail is the other axis of the same trade.
/// `Fields` opens a node per field per entity, which is what makes narrowing O(depth) at the cost
/// of a node per value; `ComponentOnly` folds every field into the component's node, which is a
/// tenth of the nodes and narrows to a component. A validation build takes the first and a shipping
/// lockstep session the second.
enum class HashDetail : u8 {
    Fields = 0,
    ComponentOnly,
};

/// One subject, lowered for one purpose.
class StateCodec {
public:
    explicit StateCodec(Allocator& allocator) noexcept : instructions_(allocator) {}

    StateCodec(const StateCodec&) = delete;
    StateCodec& operator=(const StateCodec&) = delete;

    /// Lower `subject`'s participating fields. The schema is read here and never again.
    ///
    /// Refuses: an unknown subject, an unfrozen schema (the field order would not be stable), a
    /// field whose kind has no width, and an `InternedName` field under `Save` — see the header.
    [[nodiscard]] Status compile(const StateSchema& schema, SchemaSubject subject,
                                 CodecPurpose purpose) noexcept;

    [[nodiscard]] bool compiled() const noexcept { return compiled_; }
    [[nodiscard]] CodecPurpose purpose() const noexcept { return purpose_; }
    [[nodiscard]] SchemaSubject subject() const noexcept { return subject_; }
    /// The subject's name, for a report. A literal owned by the declarer.
    [[nodiscard]] const char* subject_name() const noexcept { return subject_name_; }
    [[nodiscard]] u32 instruction_count() const noexcept {
        return static_cast<u32>(instructions_.size());
    }
    [[nodiscard]] const CodecInstruction& instruction(u32 index) const noexcept {
        return instructions_[index];
    }
    /// Bytes one row occupies when packed. Zero for a hashing codec, which packs nothing.
    [[nodiscard]] u32 packed_row_size() const noexcept { return packed_row_size_; }
    /// Fields the subject declared that this purpose excluded. Reported rather than implied: a
    /// codec that covers three of ten fields and one that covers ten of ten read identically
    /// otherwise.
    [[nodiscard]] u32 excluded_fields() const noexcept { return excluded_; }

    // --- The bulk path ---------------------------------------------------------------------------
    //
    // `base` is the first row's address, `stride` the distance between rows — an archetype column's
    // element size — and `count` the number of rows. One call per column per archetype rather than
    // one per entity, which is the requirement's "in bulk where the layout permits".

    /// Fold `count` rows into `tree`, which must have an open node to fold into.
    ///
    /// `entity_ids` may be null, in which case no Entity level is opened and the rows fold into the
    /// caller's node in order. When it is not null it has `count` entries, and each row gets an
    /// Entity node carrying its identity — `simulation-and-determinism`: "Entity identity is part
    /// of the hash."
    [[nodiscard]] Status hash_rows(StateHashTree& tree, const void* base, usize stride, u32 count,
                                   const u64* entity_ids, HashDetail detail) const noexcept;

    /// Append `count` rows' participating fields to `out`, packed tightly in instruction order.
    [[nodiscard]] Status pack_rows(const void* base, usize stride, u32 count,
                                   Array<u8>& out) const noexcept;

    /// Write `count` rows back. `bytes` must be exactly `count * packed_row_size()` long — a short
    /// buffer is refused rather than partially applied, because a half-restored world is worse than
    /// an unrestored one.
    [[nodiscard]] Status unpack_rows(Span<const u8> bytes, void* base, usize stride,
                                     u32 count) const noexcept;

private:
    /// One row's component node and its fields. Split out so `hash_rows` reads as a loop.
    [[nodiscard]] Status hash_one_row(StateHashTree& tree, const u8* address,
                                      HashDetail detail) const noexcept;

    Array<CodecInstruction> instructions_;
    SchemaSubject subject_;
    const char* subject_name_ = "";
    CodecPurpose purpose_ = CodecPurpose::Hash;
    u32 packed_row_size_ = 0;
    u32 excluded_ = 0;
    bool compiled_ = false;
};

/// The width of a field kind in bytes, or zero for one that has none.
[[nodiscard]] u8 field_kind_width(reflect::FieldKind kind) noexcept;

}  // namespace cy::determinism
