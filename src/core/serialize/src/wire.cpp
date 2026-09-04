#include <cy/core/serialize/wire.h>

#include <bit>
#include <cstring>
#include <limits>

namespace cy::serialize {
namespace {

// The float encoders below move a float through its bit pattern. That is only a defined encoding if
// the host's floats are IEEE 754 of the expected width, which every platform this engine targets
// provides — but "every platform we target" is a claim that should fail at compile time on the day
// it stops being true, not produce a file nobody can read.
static_assert(std::numeric_limits<f32>::is_iec559 && sizeof(f32) == 4,
              "the wire encoding of an f32 is its IEEE 754 bit pattern, little-endian");
static_assert(std::numeric_limits<f64>::is_iec559 && sizeof(f64) == 8,
              "the wire encoding of an f64 is its IEEE 754 bit pattern, little-endian");

/// The widest scalar the tagged form carries. A `f64`, a `u64`, and nothing larger — a wider value
/// is `Bytes`, which is copied rather than byte-swapped.
inline constexpr u32 kMaxScalarWidth = 8;

/// Copy `count` bytes and put them in little-endian order.
///
/// One function for both directions, because the transformation is its own inverse: reversing a
/// run of bytes on a big-endian host converts host order to wire order and wire order to host
/// order equally. The branch is a compile-time constant, so on the little-endian hosts this engine
/// targets it compiles to the memcpy alone.
void copy_little_endian(u8* target, const u8* source, u32 count) noexcept {
    if constexpr (std::endian::native == std::endian::big) {
        for (u32 index = 0; index < count; ++index) {
            target[index] = source[count - 1 - index];
        }
    } else {
        std::memcpy(target, source, count);
    }
}

}  // namespace

const char* wire_type_name(WireType type) noexcept {
    switch (type) {
        case WireType::Bytes:
            return "Bytes";
        case WireType::Bool:
            return "Bool";
        case WireType::I8:
            return "I8";
        case WireType::I16:
            return "I16";
        case WireType::I32:
            return "I32";
        case WireType::I64:
            return "I64";
        case WireType::U8:
            return "U8";
        case WireType::U16:
            return "U16";
        case WireType::U32:
            return "U32";
        case WireType::U64:
            return "U64";
        case WireType::F32:
            return "F32";
        case WireType::F64:
            return "F64";
        case WireType::Enum:
            return "Enum";
        case WireType::Flags:
            return "Flags";
        case WireType::LocalRef:
            return "LocalRef";
        case WireType::ExternalRef:
            return "ExternalRef";
        case WireType::Count:
            break;
    }
    return "<invalid>";
}

Status ByteWriter::write_u8(u8 value) noexcept {
    return out_->push_back(value);
}

Status ByteWriter::write_u16(u16 value) noexcept {
    const u8 bytes[2] = {static_cast<u8>(value & 0xFFU), static_cast<u8>((value >> 8U) & 0xFFU)};
    return write_bytes(bytes, sizeof(bytes));
}

Status ByteWriter::write_u32(u32 value) noexcept {
    const u8 bytes[4] = {
        static_cast<u8>(value & 0xFFU),
        static_cast<u8>((value >> 8U) & 0xFFU),
        static_cast<u8>((value >> 16U) & 0xFFU),
        static_cast<u8>((value >> 24U) & 0xFFU),
    };
    return write_bytes(bytes, sizeof(bytes));
}

Status ByteWriter::write_u64(u64 value) noexcept {
    u8 bytes[8] = {};
    for (u32 index = 0; index < 8; ++index) {
        bytes[index] = static_cast<u8>((value >> (index * 8U)) & 0xFFU);
    }
    return write_bytes(bytes, sizeof(bytes));
}

Status ByteWriter::write_bytes(const void* bytes, usize count) noexcept {
    if (count == 0) {
        return ok();
    }
    return out_->append(Span<const u8>(static_cast<const u8*>(bytes), count));
}

Status ByteWriter::write_scalar(WireType type, const void* value, u32 count) noexcept {
    // The reference kinds are composites of the integers below rather than scalars of their own, so
    // whoever writes one writes its parts. Reaching here with one is a caller error.
    if (is_reference(type)) {
        return fail(ErrorCode::InvalidArgument,
                    "a reference is written as its parts, not as one scalar");
    }
    // Anything wider than the widest scalar is opaque: it is copied, not reordered, because there
    // is no scalar interpretation to reorder it under.
    if (count == 0 || count > kMaxScalarWidth) {
        return write_bytes(value, count);
    }
    u8 wire[kMaxScalarWidth] = {};
    copy_little_endian(wire, static_cast<const u8*>(value), count);
    return write_bytes(wire, count);
}

Status ByteWriter::patch_u32(usize offset, u32 value) noexcept {
    if (offset + 4 > out_->size()) {
        return fail(ErrorCode::OutOfRange, "patch target is past the end of the buffer");
    }
    u8* target = out_->data() + offset;
    target[0] = static_cast<u8>(value & 0xFFU);
    target[1] = static_cast<u8>((value >> 8U) & 0xFFU);
    target[2] = static_cast<u8>((value >> 16U) & 0xFFU);
    target[3] = static_cast<u8>((value >> 24U) & 0xFFU);
    return ok();
}

Status ByteReader::require(usize count) const noexcept {
    if (offset_ + count > size_ || offset_ + count < offset_) {
        return fail(ErrorCode::OutOfRange, "read past the end of the serialized data");
    }
    return ok();
}

Expected<u8, Error> ByteReader::read_u8() noexcept {
    if (Status room = require(1); !room) {
        return make_unexpected(room.error());
    }
    return data_[offset_++];
}

Expected<u16, Error> ByteReader::read_u16() noexcept {
    if (Status room = require(2); !room) {
        return make_unexpected(room.error());
    }
    const u16 value =
        static_cast<u16>(static_cast<u16>(data_[offset_]) |
                         static_cast<u16>(static_cast<u16>(data_[offset_ + 1]) << 8U));
    offset_ += 2;
    return value;
}

Expected<u32, Error> ByteReader::read_u32() noexcept {
    if (Status room = require(4); !room) {
        return make_unexpected(room.error());
    }
    u32 value = 0;
    for (u32 index = 0; index < 4; ++index) {
        value |= static_cast<u32>(data_[offset_ + index]) << (index * 8U);
    }
    offset_ += 4;
    return value;
}

Expected<u64, Error> ByteReader::read_u64() noexcept {
    if (Status room = require(8); !room) {
        return make_unexpected(room.error());
    }
    u64 value = 0;
    for (u32 index = 0; index < 8; ++index) {
        value |= static_cast<u64>(data_[offset_ + index]) << (index * 8U);
    }
    offset_ += 8;
    return value;
}

Expected<Span<const u8>, Error> ByteReader::read_bytes(usize count) noexcept {
    if (Status room = require(count); !room) {
        return make_unexpected(room.error());
    }
    const Span<const u8> view(data_ + offset_, count);
    offset_ += count;
    return view;
}

Status ByteReader::skip(usize count) noexcept {
    if (Status room = require(count); !room) {
        return room;
    }
    offset_ += count;
    return ok();
}

Status decode_scalar(WireType type, const u8* bytes, u32 count, void* out) noexcept {
    if (is_reference(type)) {
        return fail(ErrorCode::InvalidArgument,
                    "a reference is decoded as its parts, not as one scalar");
    }
    if (count == 0 || count > kMaxScalarWidth) {
        std::memcpy(out, bytes, count);
        return ok();
    }
    copy_little_endian(static_cast<u8*>(out), bytes, count);
    return ok();
}

}  // namespace cy::serialize
