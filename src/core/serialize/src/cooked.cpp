#include <cy/core/serialize/cooked.h>

#include <cy/core/serialize/wire.h>

namespace cy::serialize {
namespace {

/// FNV-1a over a run of bytes. Chosen because it is short enough to read, has no table, and is
/// entirely deterministic — this number ends up in a file and is compared against one computed by a
/// different process, so "whatever the standard library's hash does" is not an option.
[[nodiscard]] u64 fnv1a(u64 seed, const void* bytes, usize count) noexcept {
    constexpr u64 kPrime = 0x0000'0100'0000'01B3ULL;
    u64 hash = seed;
    const u8* cursor = static_cast<const u8*>(bytes);
    for (usize index = 0; index < count; ++index) {
        hash ^= cursor[index];
        hash *= kPrime;
    }
    return hash;
}

[[nodiscard]] u64 fnv1a_u32(u64 seed, u32 value) noexcept {
    const u8 bytes[4] = {
        static_cast<u8>(value & 0xFFU),
        static_cast<u8>((value >> 8U) & 0xFFU),
        static_cast<u8>((value >> 16U) & 0xFFU),
        static_cast<u8>((value >> 24U) & 0xFFU),
    };
    return fnv1a(seed, bytes, sizeof(bytes));
}

}  // namespace

void BuildSchemaDigest::add_type(const reflect::TypeInfo& type, u16 schema_version) noexcept {
    constexpr u64 kOffsetBasis = 0xCBF2'9CE4'8422'2325ULL;

    u64 hash = kOffsetBasis;
    hash = fnv1a_u32(hash, type.id.value());
    hash = fnv1a_u32(hash, schema_version);
    hash = fnv1a_u32(hash, type.size);
    hash = fnv1a_u32(hash, type.alignment);
    for (u32 index = 0; index < type.field_count; ++index) {
        const reflect::FieldInfo& field = type.fields[index];
        hash = fnv1a_u32(hash, field.id.value());
        hash = fnv1a_u32(hash, static_cast<u32>(field.kind));
        hash = fnv1a_u32(hash, field.offset);
        hash = fnv1a_u32(hash, field.size);
    }

    // Combined by addition, which commutes: the cooker visits the types a scene uses and the
    // runtime walks its registry in registration order, and the two must agree without agreeing on
    // an order.
    value_ += hash;
    ++type_count_;
}

Status CookedBlock::add_column(reflect::TypeId type, u32 element_size) noexcept {
    if (!type.valid()) {
        return fail(ErrorCode::InvalidArgument, "a cooked column addresses its type by TypeId");
    }
    if (columns_.size() >= 0xFFFFU) {
        return fail(ErrorCode::OutOfRange, "a cooked block carries at most 65535 columns");
    }
    return columns_.push_back(CookedColumn{type, element_size});
}

Status CookedBlock::add_reference_site(u16 column, u32 offset) noexcept {
    if (column >= columns_.size()) {
        return fail(ErrorCode::OutOfRange,
                    "a reference site names a column this block does not have");
    }
    if (offset + kEntityReferenceBytes > columns_[column].element_size) {
        return fail(ErrorCode::OutOfRange,
                    "a reference site does not fit inside the column's element");
    }
    return sites_.push_back(ReferenceSite{column, offset});
}

usize CookedBlock::expected_payload_size() const noexcept {
    usize total = 0;
    for (const CookedColumn& column : columns_) {
        total += static_cast<usize>(column.element_size) * row_count_;
    }
    return total;
}

Expected<Span<const u8>, Error> CookedBlock::column_bytes(usize index) const noexcept {
    if (index >= columns_.size()) {
        return fail(ErrorCode::OutOfRange, "no such column in this block");
    }
    usize offset = 0;
    for (usize position = 0; position < index; ++position) {
        offset += static_cast<usize>(columns_[position].element_size) * row_count_;
    }
    const usize length = static_cast<usize>(columns_[index].element_size) * row_count_;
    if (offset + length > payload_.size()) {
        return fail(ErrorCode::OutOfRange, "the block's payload is shorter than its columns");
    }
    return payload_.subspan(offset, length);
}

void CookedBlock::clear() noexcept {
    columns_.clear();
    sites_.clear();
    payload_ = {};
    row_count_ = 0;
}

Status CookedWriter::begin_stream(u64 build_schema) noexcept {
    if (began_) {
        return fail(ErrorCode::InvalidArgument, "the stream header has already been written");
    }
    ByteWriter writer(*out_);
    if (Status written = writer.write_u32(kCookedMagic); !written) {
        return written;
    }
    if (Status written = writer.write_u16(kCookedFormatVersion); !written) {
        return written;
    }
    if (Status written = writer.write_u16(0); !written) {  // flags, reserved
        return written;
    }
    if (Status written = writer.write_u64(build_schema); !written) {
        return written;
    }
    block_count_offset_ = out_->size();
    if (Status written = writer.write_u32(0); !written) {
        return written;
    }
    began_ = true;
    return ok();
}

Status CookedWriter::write_block(const CookedBlock& block) noexcept {
    if (!began_) {
        return fail(ErrorCode::InvalidArgument, "begin_stream() has not been called");
    }
    if (block.payload().size() != block.expected_payload_size()) {
        return fail(ErrorCode::InvalidArgument,
                    "the block's payload does not match its declared columns and row count");
    }

    ByteWriter writer(*out_);
    if (Status written = writer.write_u32(block.row_count()); !written) {
        return written;
    }
    if (Status written = writer.write_u32(static_cast<u32>(block.columns().size())); !written) {
        return written;
    }
    if (Status written = writer.write_u32(static_cast<u32>(block.reference_sites().size()));
        !written) {
        return written;
    }
    if (Status written = writer.write_u32(static_cast<u32>(block.payload().size())); !written) {
        return written;
    }
    for (const CookedColumn& column : block.columns()) {
        if (Status written = writer.write_u32(column.type.value()); !written) {
            return written;
        }
        if (Status written = writer.write_u32(column.element_size); !written) {
            return written;
        }
    }
    for (const ReferenceSite& site : block.reference_sites()) {
        if (Status written = writer.write_u16(site.column); !written) {
            return written;
        }
        if (Status written = writer.write_u16(0); !written) {  // reserved
            return written;
        }
        if (Status written = writer.write_u32(site.offset); !written) {
            return written;
        }
    }
    if (Status written = writer.write_bytes(block.payload().data(), block.payload().size());
        !written) {
        return written;
    }
    ++block_count_;
    return ok();
}

Status CookedWriter::end_stream() noexcept {
    if (!began_) {
        return fail(ErrorCode::InvalidArgument, "begin_stream() has not been called");
    }
    ByteWriter writer(*out_);
    return writer.patch_u32(block_count_offset_, block_count_);
}

Status CookedReader::read_header(u64 expected_build_schema) noexcept {
    ByteReader reader(data_, size_);
    const Expected<u32, Error> magic = reader.read_u32();
    if (!magic) {
        return make_unexpected(magic.error());
    }
    if (magic.value() != kCookedMagic) {
        return fail(ErrorCode::InvalidArgument, "not a cooked stream: wrong magic");
    }
    const Expected<u16, Error> version = reader.read_u16();
    if (!version) {
        return make_unexpected(version.error());
    }
    if (version.value() != kCookedFormatVersion) {
        return fail(ErrorCode::Unsupported,
                    "cooked stream was written by a different build of the format");
    }
    const Expected<u16, Error> flags = reader.read_u16();
    if (!flags) {
        return make_unexpected(flags.error());
    }
    const Expected<u64, Error> schema = reader.read_u64();
    if (!schema) {
        return make_unexpected(schema.error());
    }
    const Expected<u32, Error> blocks = reader.read_u32();
    if (!blocks) {
        return make_unexpected(blocks.error());
    }

    build_schema_ = schema.value();
    if (build_schema_ != expected_build_schema) {
        // Fatal, and fatal here rather than at the first row that reads wrong: cooked data has no
        // evolution mechanism, so a schema that does not match means the packed bytes mean
        // something other than what this build would read them as.
        return fail(ErrorCode::Unsupported,
                    "cooked data was produced against a different build schema");
    }
    block_count_ = blocks.value();
    offset_ = reader.offset();
    header_read_ = true;
    return ok();
}

Status CookedReader::next_block(CookedBlock& out) noexcept {
    if (!header_read_) {
        return fail(ErrorCode::InvalidArgument, "read_header() has not been called");
    }
    if (blocks_read_ >= block_count_) {
        return fail(ErrorCode::NotFound, "no further blocks");
    }

    ByteReader reader(data_ + offset_, size_ - offset_);
    const Expected<u32, Error> rows = reader.read_u32();
    if (!rows) {
        return make_unexpected(rows.error());
    }
    const Expected<u32, Error> columns = reader.read_u32();
    if (!columns) {
        return make_unexpected(columns.error());
    }
    const Expected<u32, Error> sites = reader.read_u32();
    if (!sites) {
        return make_unexpected(sites.error());
    }
    const Expected<u32, Error> payload_size = reader.read_u32();
    if (!payload_size) {
        return make_unexpected(payload_size.error());
    }

    out.clear();
    out.set_row_count(rows.value());
    for (u32 index = 0; index < columns.value(); ++index) {
        const Expected<u32, Error> type = reader.read_u32();
        if (!type) {
            return make_unexpected(type.error());
        }
        const Expected<u32, Error> element_size = reader.read_u32();
        if (!element_size) {
            return make_unexpected(element_size.error());
        }
        if (Status added = out.add_column(reflect::TypeId(type.value()), element_size.value());
            !added) {
            return added;
        }
    }
    for (u32 index = 0; index < sites.value(); ++index) {
        const Expected<u16, Error> column = reader.read_u16();
        if (!column) {
            return make_unexpected(column.error());
        }
        if (Status skipped = reader.skip(2); !skipped) {  // reserved
            return skipped;
        }
        const Expected<u32, Error> offset = reader.read_u32();
        if (!offset) {
            return make_unexpected(offset.error());
        }
        if (Status added = out.add_reference_site(column.value(), offset.value()); !added) {
            return added;
        }
    }

    const Expected<Span<const u8>, Error> payload = reader.read_bytes(payload_size.value());
    if (!payload) {
        return make_unexpected(payload.error());
    }
    out.set_payload(payload.value());
    if (out.payload().size() != out.expected_payload_size()) {
        return fail(ErrorCode::InvalidArgument,
                    "the block's payload does not match its declared columns and row count");
    }

    offset_ += reader.offset();
    ++blocks_read_;
    return ok();
}

}  // namespace cy::serialize
