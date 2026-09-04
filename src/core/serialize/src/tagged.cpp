#include <cy/core/serialize/tagged.h>

#include <utility>

namespace cy::serialize {
namespace {

/// The most bytes one tagged field may carry. The length is a `u16` on the wire, which is generous
/// for a scalar and deliberately too small for bulk data — bulk data is the cooked form's job, and
/// a limit that says so is better than one that lets a caller discover it at 65 537 bytes.
inline constexpr u32 kMaxFieldBytes = 0xFFFFU;

}  // namespace

Status TaggedWriter::begin_stream() noexcept {
    if (began_) {
        return fail(ErrorCode::InvalidArgument, "the stream header has already been written");
    }
    if (Status written = writer_.write_u32(kTaggedMagic); !written) {
        return written;
    }
    if (Status written = writer_.write_u16(kTaggedFormatVersion); !written) {
        return written;
    }
    if (Status written = writer_.write_u16(0); !written) {  // flags, reserved
        return written;
    }
    chunk_count_offset_ = out_->size();
    if (Status written = writer_.write_u32(0); !written) {
        return written;
    }
    began_ = true;
    return ok();
}

Status TaggedWriter::begin_chunk(u32 tag) noexcept {
    if (!began_) {
        return fail(ErrorCode::InvalidArgument, "begin_stream() has not been called");
    }
    if (in_chunk_) {
        return fail(ErrorCode::InvalidArgument, "a chunk is already open");
    }
    if (Status written = writer_.write_u32(tag); !written) {
        return written;
    }
    chunk_size_offset_ = out_->size();
    if (Status written = writer_.write_u32(0); !written) {
        return written;
    }
    in_chunk_ = true;
    return ok();
}

Status TaggedWriter::end_chunk() noexcept {
    if (!in_chunk_) {
        return fail(ErrorCode::InvalidArgument, "no chunk is open");
    }
    const usize payload = out_->size() - (chunk_size_offset_ + 4);
    if (payload > 0xFFFF'FFFFULL) {
        return fail(ErrorCode::OutOfRange, "chunk payload exceeds four gigabytes");
    }
    if (Status patched = writer_.patch_u32(chunk_size_offset_, static_cast<u32>(payload));
        !patched) {
        return patched;
    }
    in_chunk_ = false;
    ++chunk_count_;
    return ok();
}

Status TaggedWriter::write_record(const ValueRecord& record) noexcept {
    if (!in_chunk_) {
        return fail(ErrorCode::InvalidArgument, "records are written inside a chunk");
    }
    if (!record.type().valid()) {
        return fail(ErrorCode::InvalidArgument, "a record addresses its type by TypeId");
    }
    if (record.size() > 0xFFFFU) {
        return fail(ErrorCode::OutOfRange, "a record carries at most 65535 fields");
    }

    if (Status written = writer_.write_u32(record.type().value()); !written) {
        return written;
    }
    if (Status written = writer_.write_u16(record.schema_version()); !written) {
        return written;
    }
    if (Status written = writer_.write_u16(static_cast<u16>(record.size())); !written) {
        return written;
    }
    const usize payload_size_offset = out_->size();
    if (Status written = writer_.write_u32(0); !written) {
        return written;
    }

    for (const FieldValue& value : record.fields()) {
        if (value.size > kMaxFieldBytes) {
            return fail(ErrorCode::OutOfRange,
                        "a tagged field carries at most 65535 bytes; bulk data belongs in the "
                        "cooked form");
        }
        if (Status written = writer_.write_u32(value.id.value()); !written) {
            return written;
        }
        if (Status written = writer_.write_u8(static_cast<u8>(value.wire)); !written) {
            return written;
        }
        if (Status written = writer_.write_u8(0); !written) {  // reserved
            return written;
        }
        if (Status written = writer_.write_u16(static_cast<u16>(value.size)); !written) {
            return written;
        }
        const Span<const u8> bytes = record.bytes(value);
        if (Status written = writer_.write_bytes(bytes.data(), bytes.size()); !written) {
            return written;
        }
    }

    const usize payload = out_->size() - (payload_size_offset + 4);
    if (payload > 0xFFFF'FFFFULL) {
        return fail(ErrorCode::OutOfRange, "record payload exceeds four gigabytes");
    }
    return writer_.patch_u32(payload_size_offset, static_cast<u32>(payload));
}

Status TaggedWriter::write_object(const reflect::TypeInfo& type, const void* object,
                                  Purpose purpose, u16 schema_version) noexcept {
    ValueRecord record(out_->allocator());
    if (Status built = record_from_object(type, object, purpose, record); !built) {
        return built;
    }
    record.set_schema_version(schema_version);
    return write_record(record);
}

Status TaggedWriter::end_stream() noexcept {
    if (!began_) {
        return fail(ErrorCode::InvalidArgument, "begin_stream() has not been called");
    }
    if (in_chunk_) {
        return fail(ErrorCode::InvalidArgument, "a chunk is still open");
    }
    return writer_.patch_u32(chunk_count_offset_, chunk_count_);
}

Status TaggedReader::read_header() noexcept {
    const Expected<u32, Error> magic = reader_.read_u32();
    if (!magic) {
        return make_unexpected(magic.error());
    }
    if (magic.value() != kTaggedMagic) {
        return fail(ErrorCode::InvalidArgument, "not a tagged stream: wrong magic");
    }
    const Expected<u16, Error> version = reader_.read_u16();
    if (!version) {
        return make_unexpected(version.error());
    }
    if (version.value() > kTaggedFormatVersion) {
        return fail(ErrorCode::Unsupported,
                    "tagged stream was written by a newer build of the format");
    }
    const Expected<u16, Error> flags = reader_.read_u16();
    if (!flags) {
        return make_unexpected(flags.error());
    }
    const Expected<u32, Error> chunks = reader_.read_u32();
    if (!chunks) {
        return make_unexpected(chunks.error());
    }
    chunk_count_ = chunks.value();
    header_read_ = true;
    return ok();
}

Expected<TaggedChunk, Error> TaggedReader::next_chunk() noexcept {
    if (!header_read_) {
        return fail(ErrorCode::InvalidArgument, "read_header() has not been called");
    }
    if (chunks_read_ >= chunk_count_) {
        return fail(ErrorCode::NotFound, "no further chunks");
    }
    const Expected<u32, Error> tag = reader_.read_u32();
    if (!tag) {
        return make_unexpected(tag.error());
    }
    const Expected<u32, Error> size = reader_.read_u32();
    if (!size) {
        return make_unexpected(size.error());
    }
    const Expected<Span<const u8>, Error> payload = reader_.read_bytes(size.value());
    if (!payload) {
        return make_unexpected(payload.error());
    }
    ++chunks_read_;
    return TaggedChunk{tag.value(), payload.value()};
}

Expected<TaggedRecordHeader, Error> read_record_header(ByteReader& reader) noexcept {
    const Expected<u32, Error> type = reader.read_u32();
    if (!type) {
        return make_unexpected(type.error());
    }
    const Expected<u16, Error> schema = reader.read_u16();
    if (!schema) {
        return make_unexpected(schema.error());
    }
    const Expected<u16, Error> count = reader.read_u16();
    if (!count) {
        return make_unexpected(count.error());
    }
    const Expected<u32, Error> payload = reader.read_u32();
    if (!payload) {
        return make_unexpected(payload.error());
    }
    TaggedRecordHeader header;
    header.type = reflect::TypeId(type.value());
    header.schema_version = schema.value();
    header.field_count = count.value();
    header.payload_size = payload.value();
    return header;
}

Status read_record(ByteReader& reader, ValueRecord& out) noexcept {
    const Expected<TaggedRecordHeader, Error> header = read_record_header(reader);
    if (!header) {
        return make_unexpected(header.error());
    }
    const usize payload_end = reader.offset() + header->payload_size;

    out.clear();
    out.set_type(header->type);
    out.set_schema_version(header->schema_version);

    for (u32 index = 0; index < header->field_count; ++index) {
        const Expected<u32, Error> id = reader.read_u32();
        if (!id) {
            return make_unexpected(id.error());
        }
        const Expected<u8, Error> wire = reader.read_u8();
        if (!wire) {
            return make_unexpected(wire.error());
        }
        if (Status skipped = reader.skip(1); !skipped) {  // reserved
            return skipped;
        }
        const Expected<u16, Error> size = reader.read_u16();
        if (!size) {
            return make_unexpected(size.error());
        }
        const Expected<Span<const u8>, Error> bytes = reader.read_bytes(size.value());
        if (!bytes) {
            return make_unexpected(bytes.error());
        }
        if (wire.value() >= static_cast<u8>(WireType::Count)) {
            return fail(ErrorCode::Unsupported,
                        "field carries a wire type this build does not define");
        }
        if (Status stored =
                out.set(reflect::FieldId(id.value()), static_cast<WireType>(wire.value()),
                        bytes->data(), static_cast<u32>(bytes->size()));
            !stored) {
            return stored;
        }
    }

    // The declared payload size is the authority, not the sum of the fields: a writer that added a
    // trailer this build does not know about should be stepped over, not rejected.
    if (reader.offset() > payload_end) {
        return fail(ErrorCode::InvalidArgument, "record fields overran the record's payload");
    }
    return reader.skip(payload_end - reader.offset());
}

Status read_records(Span<const u8> payload, Array<ValueRecord>& out) noexcept {
    ByteReader reader(payload.data(), payload.size());
    while (!reader.empty()) {
        ValueRecord record(out.allocator());
        if (Status read = read_record(reader, record); !read) {
            return read;
        }
        if (Status appended = out.push_back(std::move(record)); !appended) {
            return appended;
        }
    }
    return ok();
}

}  // namespace cy::serialize
