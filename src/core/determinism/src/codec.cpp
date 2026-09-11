// Generated state codecs: one compiled plan per subject per purpose. M9 task 2.4.

#include <cy/core/determinism/codec.h>

#include <cstring>

namespace cy::determinism {
namespace {

[[nodiscard]] const u8* row_at(const void* base, usize stride, u32 index) noexcept {
    return static_cast<const u8*>(base) + (static_cast<usize>(index) * stride);
}

[[nodiscard]] u8* mutable_row_at(void* base, usize stride, u32 index) noexcept {
    return static_cast<u8*>(base) + (static_cast<usize>(index) * stride);
}

}  // namespace

const char* codec_purpose_name(CodecPurpose purpose) noexcept {
    switch (purpose) {
        case CodecPurpose::Hash:
            return "Hash";
        case CodecPurpose::RollbackPack:
            return "RollbackPack";
        case CodecPurpose::CheckpointPack:
            return "CheckpointPack";
        case CodecPurpose::Save:
            return "Save";
    }
    return "Hash";
}

u8 field_kind_width(reflect::FieldKind kind) noexcept {
    switch (kind) {
        case reflect::FieldKind::Bool:
        case reflect::FieldKind::I8:
        case reflect::FieldKind::U8:
            return 1;
        case reflect::FieldKind::I16:
        case reflect::FieldKind::U16:
            return 2;
        case reflect::FieldKind::I32:
        case reflect::FieldKind::U32:
        case reflect::FieldKind::F32:
            return 4;
        case reflect::FieldKind::I64:
        case reflect::FieldKind::U64:
        case reflect::FieldKind::F64:
            return 8;
        case reflect::FieldKind::Enum:
        case reflect::FieldKind::Flags:
        case reflect::FieldKind::Unsupported:
            break;
    }
    // Enum and Flags carry their width in the reflected descriptor rather than in the kind, and a
    // schema stores the underlying integer kind instead — `StateSchema::declare_reflected()` does
    // that translation. Reaching here is a declaration that should have been refused.
    return 0;
}

Status StateCodec::compile(const StateSchema& schema, SchemaSubject subject,
                           CodecPurpose purpose) noexcept {
    if (!schema.frozen()) {
        return fail(ErrorCode::Unavailable,
                    "determinism: a codec is compiled from a frozen schema; an unfrozen one has "
                    "not yet fixed the order its fields fold in");
    }
    const SubjectSchema* declaration = schema.find(subject);
    if (declaration == nullptr) {
        return fail(ErrorCode::NotFound, "determinism: no state schema for this subject");
    }

    instructions_.clear();
    packed_row_size_ = 0;
    excluded_ = 0;
    compiled_ = false;
    subject_ = subject;
    subject_name_ = declaration->name;
    purpose_ = purpose;

    for (const StateField& field : schema.fields_of(*declaration)) {
        if (!field_participates(field.classification, purpose)) {
            ++excluded_;
            continue;
        }
        if (purpose == CodecPurpose::Save && field.encoding == StateEncoding::InternedName) {
            // A `Name` packs as its intern index, which is assigned in interning order and is not
            // stable across runs. Correct for a rollback ring in this process; meaningless in a
            // file. See the header.
            instructions_.clear();
            return fail(ErrorCode::Unsupported,
                        "determinism: an InternedName field cannot be packed for Save — its index "
                        "is process-local; the versioned save encoding writes its text");
        }
        const u8 width = field_kind_width(field.kind);
        if (width == 0) {
            instructions_.clear();
            return fail(ErrorCode::Unsupported,
                        "determinism: a state field's kind has no width the codec can pack");
        }
        CodecInstruction instruction;
        instruction.field_id = field.id;
        instruction.name = field.name;
        instruction.offset = field.offset;
        instruction.width = width;
        instruction.kind = field.kind;
        instruction.encoding = field.encoding;
        if (Status added = instructions_.push_back(instruction); !added) {
            instructions_.clear();
            return added;
        }
        if (purpose != CodecPurpose::Hash) {
            packed_row_size_ += width;
        }
    }

    compiled_ = true;
    return ok();
}

namespace {

/// One field of one row, folded into the open node. Split out of `hash_rows` so that the hot loop
/// reads as a loop: the branch on detail and the reconstruction of a `StateField` are this
/// function's business and not the caller's.
[[nodiscard]] Status hash_one_field(StateHashTree& tree, const CodecInstruction& instruction,
                                    const u8* address, HashDetail detail) noexcept {
    StateField field;
    field.name = instruction.name;
    field.id = instruction.field_id;
    field.offset = instruction.offset;
    field.kind = instruction.kind;
    field.encoding = instruction.encoding;

    if (detail == HashDetail::ComponentOnly) {
        hash_field(tree, field, address);
        return ok();
    }
    if (Status opened = tree.begin(HashLevel::Field, instruction.field_id, instruction.name);
        !opened) {
        return opened;
    }
    hash_field(tree, field, address);
    return tree.end();
}

}  // namespace

Status StateCodec::hash_one_row(StateHashTree& tree, const u8* address,
                                HashDetail detail) const noexcept {
    if (Status opened = tree.begin(HashLevel::Component, subject_.value, subject_name_); !opened) {
        return opened;
    }
    // THE HOT LOOP. No schema, no reflection, no classification test: the instruction list already
    // is the answer to all three.
    for (const CodecInstruction& instruction : instructions_) {
        if (Status folded = hash_one_field(tree, instruction, address, detail); !folded) {
            return folded;
        }
    }
    return tree.end();
}

Status StateCodec::hash_rows(StateHashTree& tree, const void* base, usize stride, u32 count,
                             const u64* entity_ids, HashDetail detail) const noexcept {
    if (!compiled_ || purpose_ != CodecPurpose::Hash) {
        return fail(ErrorCode::Unavailable, "determinism: this codec was not compiled for hashing");
    }
    if (!tree.open()) {
        return fail(ErrorCode::InvalidArgument,
                    "determinism: hash_rows folds into an open node; the caller opens the "
                    "archetype or subsystem level it belongs under");
    }
    if (count != 0 && base == nullptr) {
        return fail(ErrorCode::InvalidArgument, "determinism: hash_rows was given no rows to read");
    }

    for (u32 row = 0; row < count; ++row) {
        const u8* address = row_at(base, stride, row);
        if (entity_ids == nullptr) {
            if (Status hashed = hash_one_row(tree, address, detail); !hashed) {
                return hashed;
            }
            continue;
        }
        // `simulation-and-determinism`: "Entity identity is part of the hash." One node per row,
        // carrying the identity the divergence report has to name.
        if (Status opened = tree.begin(HashLevel::Entity, entity_ids[row], ""); !opened) {
            return opened;
        }
        if (Status hashed = hash_one_row(tree, address, detail); !hashed) {
            return hashed;
        }
        if (Status closed = tree.end(); !closed) {
            return closed;
        }
    }
    return ok();
}

Status StateCodec::pack_rows(const void* base, usize stride, u32 count,
                             Array<u8>& out) const noexcept {
    if (!compiled_ || purpose_ == CodecPurpose::Hash) {
        return fail(ErrorCode::Unavailable, "determinism: this codec was not compiled for packing");
    }
    if (count != 0 && base == nullptr) {
        return fail(ErrorCode::InvalidArgument, "determinism: pack_rows was given no rows to read");
    }
    const usize wanted = out.size() + (static_cast<usize>(count) * packed_row_size_);
    if (Status reserved = out.reserve(wanted); !reserved) {
        return reserved;
    }
    const usize mark = out.size();
    for (u32 row = 0; row < count; ++row) {
        const u8* address = row_at(base, stride, row);
        for (const CodecInstruction& instruction : instructions_) {
            for (u8 byte = 0; byte < instruction.width; ++byte) {
                if (Status pushed = out.push_back(address[instruction.offset + byte]); !pushed) {
                    (void)out.resize(mark);
                    return pushed;
                }
            }
        }
    }
    return ok();
}

Status StateCodec::unpack_rows(Span<const u8> bytes, void* base, usize stride,
                               u32 count) const noexcept {
    if (!compiled_ || purpose_ == CodecPurpose::Hash) {
        return fail(ErrorCode::Unavailable,
                    "determinism: this codec was not compiled for unpacking");
    }
    if (bytes.size() != static_cast<usize>(count) * packed_row_size_) {
        // Refused rather than partially applied. A world restored from a short buffer is a world
        // whose first half is one tick and whose second half is another, and it would hash as a
        // divergence somewhere unrelated to the actual defect.
        return fail(ErrorCode::InvalidArgument,
                    "determinism: the packed buffer is not this many rows of this codec");
    }
    if (count != 0 && base == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "determinism: unpack_rows was given nowhere to write");
    }
    usize cursor = 0;
    for (u32 row = 0; row < count; ++row) {
        u8* address = mutable_row_at(base, stride, row);
        for (const CodecInstruction& instruction : instructions_) {
            std::memcpy(address + instruction.offset, bytes.data() + cursor, instruction.width);
            cursor += instruction.width;
        }
    }
    return ok();
}

}  // namespace cy::determinism
