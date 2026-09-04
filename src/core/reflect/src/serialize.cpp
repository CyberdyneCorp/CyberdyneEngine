// The round-trip encoding. Task 1.2.5.
//
// Little-endian, written byte by byte rather than by memcpy of a native integer, so that a golden
// committed on one machine is read identically on another. The engine has no big-endian target
// today; writing the encoding explicitly costs nothing here and means the first one does not
// silently produce different files.

#include <cy/core/reflect/serialize.h>

#include <cstring>
#include <new>

namespace cy::reflect {
namespace {

void put_u16(u8* out, u16 value) noexcept {
    out[0] = static_cast<u8>(value & 0xFFu);
    out[1] = static_cast<u8>((value >> 8) & 0xFFu);
}

void put_u32(u8* out, u32 value) noexcept {
    out[0] = static_cast<u8>(value & 0xFFu);
    out[1] = static_cast<u8>((value >> 8) & 0xFFu);
    out[2] = static_cast<u8>((value >> 16) & 0xFFu);
    out[3] = static_cast<u8>((value >> 24) & 0xFFu);
}

u16 get_u16(const u8* in) noexcept {
    return static_cast<u16>(static_cast<u16>(in[0]) | static_cast<u16>(in[1] << 8));
}

u32 get_u32(const u8* in) noexcept {
    return static_cast<u32>(in[0]) | (static_cast<u32>(in[1]) << 8) |
           (static_cast<u32>(in[2]) << 16) | (static_cast<u32>(in[3]) << 24);
}

constexpr usize field_header_size = 8;

// A field's bytes are copied verbatim. Every FieldKind at M1 is a fixed-width scalar whose
// in-memory representation is its serialized one on every target the engine builds for; the
// floating-point policy `simulation-and-determinism` will fix at M9 governs arithmetic, not
// storage. is_scalar() is the guard that makes the first non-scalar kind fail loudly here.
bool copyable(FieldKind kind) noexcept {
    return is_scalar(kind);
}

}  // namespace

ByteBuffer::~ByteBuffer() {
    delete[] data_;
}

Status ByteBuffer::reserve(usize wanted) {
    if (wanted <= capacity_) {
        return ok();
    }
    usize capacity = (capacity_ == 0) ? 64 : capacity_ * 2;
    while (capacity < wanted) {
        capacity *= 2;
    }
    auto* grown = new (std::nothrow) u8[capacity];
    if (grown == nullptr) {
        return fail(ErrorCode::OutOfMemory, "ByteBuffer could not grow");
    }
    if (size_ != 0) {
        std::memcpy(grown, data_, size_);
    }
    delete[] data_;
    data_ = grown;
    capacity_ = capacity;
    return ok();
}

Status ByteBuffer::append(const void* bytes, usize count) {
    if (count == 0) {
        return ok();
    }
    if (auto grown = reserve(size_ + count); !grown) {
        return grown;
    }
    std::memcpy(data_ + size_, bytes, count);
    size_ += count;
    return ok();
}

Status write_record(const TypeInfo& type, const void* object, ByteBuffer& out) {
    if (object == nullptr) {
        return fail(ErrorCode::InvalidArgument, "write_record: null object");
    }

    // The payload is built first so that the header can state its size; a record whose size is only
    // known after the fact is a record a reader cannot skip.
    ByteBuffer payload;
    u16 written = 0;
    for (u32 index = 0; index < type.field_count; ++index) {
        const FieldInfo& field = type.fields[index];
        if (field.attributes.transient()) {
            continue;
        }
        if (!copyable(field.kind)) {
            return fail(ErrorCode::Unsupported,
                        "write_record: a field kind this encoding cannot carry");
        }
        if (field.size > 0xFFFFu) {
            return fail(ErrorCode::OutOfRange, "write_record: field wider than the record format");
        }

        u8 header[field_header_size];
        put_u32(header, field.id.value());
        header[4] = static_cast<u8>(field.kind);
        header[5] = 0;
        put_u16(header + 6, static_cast<u16>(field.size));
        if (auto appended = payload.append(header, field_header_size); !appended) {
            return appended;
        }
        const auto* bytes = static_cast<const u8*>(object) + field.offset;
        if (auto appended = payload.append(bytes, field.size); !appended) {
            return appended;
        }
        ++written;
    }

    u8 header[RecordHeader::header_size];
    put_u32(header, type.id.value());
    put_u32(header + 4, static_cast<u32>(payload.size()));
    put_u16(header + 8, written);
    put_u16(header + 10, 0);
    if (auto appended = out.append(header, RecordHeader::header_size); !appended) {
        return appended;
    }
    return out.append(payload.data(), payload.size());
}

Expected<RecordHeader, Error> peek_record(const u8* data, usize size) {
    if (data == nullptr || size < RecordHeader::header_size) {
        return fail(ErrorCode::BufferTooSmall, "peek_record: fewer bytes than a record header");
    }
    RecordHeader header;
    header.type = TypeId(get_u32(data));
    header.payload_size = get_u32(data + 4);
    header.field_count = get_u16(data + 8);
    if (header.total_size() > size) {
        return fail(ErrorCode::BufferTooSmall, "peek_record: record extends past the buffer");
    }
    return header;
}

Status read_record(const FieldIndex& fields, const u8* data, usize size, void* object) {
    const TypeInfo* type = fields.type();
    if (type == nullptr) {
        return fail(ErrorCode::InvalidArgument, "read_record: the field index was never built");
    }
    if (object == nullptr) {
        return fail(ErrorCode::InvalidArgument, "read_record: null object");
    }
    auto header = peek_record(data, size);
    if (!header) {
        return Unexpected<Error>(header.error());
    }
    if (header->type != type->id) {
        return fail(ErrorCode::InvalidArgument,
                    "read_record: the record was written for a different TypeId");
    }

    // Walked as an offset into the payload rather than as two pointers, so that every bound is a
    // comparison of unsigned lengths and no cast is needed to make one true.
    //
    // One pass, one hash probe per field. M1 called TypeInfo::find_field() here, which scans, so
    // the cost was the record's field count times the type's; the M2 spec delta requires this to be
    // linear in the record, and the index is how it becomes so.
    const u8* payload = data + RecordHeader::header_size;
    const usize limit = header->payload_size;
    usize position = 0;

    for (u32 index = 0; index < static_cast<u32>(header->field_count); ++index) {
        if (position + field_header_size > limit) {
            return fail(ErrorCode::Io, "read_record: truncated field header");
        }
        const u8* entry = payload + position;
        const FieldId id{get_u32(entry)};
        const auto kind = static_cast<FieldKind>(entry[4]);
        const usize width = get_u16(entry + 6);
        position += field_header_size;
        if (position + width > limit) {
            return fail(ErrorCode::Io, "read_record: truncated field value");
        }

        // A field the type no longer declares is skipped rather than rejected. Its number is
        // tombstoned in the manifest and will never be issued again, so skipping it can never mean
        // silently writing one field's bytes into another's.
        if (const FieldInfo* field = fields.find(id); field != nullptr) {
            if (field->kind != kind || static_cast<usize>(field->size) != width) {
                return fail(ErrorCode::InvalidArgument,
                            "read_record: a field's recorded kind or width no longer matches the "
                            "type; this needs a migration, not a reinterpretation");
            }
            std::memcpy(static_cast<u8*>(object) + field->offset, payload + position, width);
        }
        position += width;
    }
    return ok();
}

Status read_record(const TypeInfo& type, const u8* data, usize size, void* object) {
    FieldIndex fields;
    if (auto built = fields.build(type); !built) {
        return built;
    }
    return read_record(fields, data, size, object);
}

Status write_opaque(const u8* data, usize size, ByteBuffer& out) {
    auto header = peek_record(data, size);
    if (!header) {
        return Unexpected<Error>(header.error());
    }
    return out.append(data, header->total_size());
}

}  // namespace cy::reflect
