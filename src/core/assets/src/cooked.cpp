#include <cy/core/assets/cooked.h>

#include <cstring>
#include <string_view>

namespace cy::assets {
namespace {

/// The header's byte layout. Written out rather than memcpy'd from the struct: a struct's padding
/// is a compiler artefact and this is a file format.
///
///   0  8   magic
///   8  4   format version, little-endian
///  12  2   asset kind, little-endian
///  14  2   reserved, zero
///  16 32   content hash
///  48  8   payload size, little-endian
///  56 24   variant key, NUL-terminated
///
/// Eighty bytes in total. The variant key is written as characters rather than as a hash of them,
/// for the reason `identity.h` gives: a collision in this field would serve the wrong platform's
/// asset with no diagnostic at all.
constexpr usize kMagicOffset = 0;
constexpr usize kVersionOffset = 8;
constexpr usize kKindOffset = 12;
constexpr usize kHashOffset = 16;
constexpr usize kPayloadSizeOffset = 48;
constexpr usize kVariantOffset = 56;
constexpr usize kVariantBytes = kCookedHeaderBytes - kVariantOffset;

static_assert(kVariantBytes >= VariantKey::kCapacity + 1,
              "the header's tail must hold a variant key and its terminator");

void write_u16(u8* out, u16 value) noexcept {
    out[0] = static_cast<u8>(value & 0xFFU);
    out[1] = static_cast<u8>((value >> 8U) & 0xFFU);
}

void write_u32(u8* out, u32 value) noexcept {
    for (u32 index = 0; index < 4; ++index) {
        out[index] = static_cast<u8>((value >> (index * 8U)) & 0xFFU);
    }
}

void write_u64(u8* out, u64 value) noexcept {
    for (u32 index = 0; index < 8; ++index) {
        out[index] = static_cast<u8>((value >> (index * 8U)) & 0xFFU);
    }
}

[[nodiscard]] u16 read_u16(const u8* data) noexcept {
    return static_cast<u16>(static_cast<u16>(data[0]) |
                            static_cast<u16>(static_cast<u16>(data[1]) << 8U));
}

[[nodiscard]] u32 read_u32(const u8* data) noexcept {
    u32 value = 0;
    for (u32 index = 0; index < 4; ++index) {
        value |= static_cast<u32>(data[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] u64 read_u64(const u8* data) noexcept {
    u64 value = 0;
    for (u32 index = 0; index < 8; ++index) {
        value |= static_cast<u64>(data[index]) << (index * 8U);
    }
    return value;
}

}  // namespace

Status write_cooked_asset(AssetKind kind, VariantKey variant, Span<const u8> payload,
                          Array<u8>& out) noexcept {
    ContentHasher hasher;
    if (!payload.empty()) {
        hasher.update(payload.data(), payload.size());
    }
    const ContentHash hash = hasher.finish();

    u8 header[kCookedHeaderBytes] = {};
    std::memcpy(header + kMagicOffset, kCookedMagic, sizeof(kCookedMagic));
    write_u32(header + kVersionOffset, kCookedFormatVersion);
    write_u16(header + kKindOffset, static_cast<u16>(kind));
    std::memcpy(header + kHashOffset, hash.bytes, ContentHash::kByteLength);
    write_u64(header + kPayloadSizeOffset, payload.size());

    const std::string_view text = variant.view();
    if (text.size() >= kVariantBytes) {
        return fail(ErrorCode::OutOfRange, "the variant key does not fit in a cooked header");
    }
    std::memcpy(header + kVariantOffset, text.data(), text.size());

    if (Status written = out.append(Span<const u8>(header, sizeof(header))); !written) {
        return written;
    }
    return out.append(payload);
}

Expected<CookedAssetHeader, Error> read_cooked_header(const u8* data, usize size) noexcept {
    if (data == nullptr || size < kCookedHeaderBytes) {
        return fail(ErrorCode::OutOfRange, "shorter than a cooked asset header");
    }
    if (std::memcmp(data + kMagicOffset, kCookedMagic, sizeof(kCookedMagic)) != 0) {
        return fail(ErrorCode::InvalidArgument, "not a cooked asset: wrong magic");
    }

    CookedAssetHeader header;
    header.format_version = read_u32(data + kVersionOffset);
    if (header.format_version > kCookedFormatVersion) {
        return fail(ErrorCode::Unsupported,
                    "the cooked asset's header version is newer than this build supports");
    }
    const u16 kind = read_u16(data + kKindOffset);
    if (kind > static_cast<u16>(AssetKind::Binary)) {
        return fail(ErrorCode::Unsupported, "an asset kind this build does not define");
    }
    header.kind = static_cast<AssetKind>(kind);
    std::memcpy(header.content.bytes, data + kHashOffset, ContentHash::kByteLength);
    header.payload_size = read_u64(data + kPayloadSizeOffset);

    // The variant key is NUL-terminated within its field, and a field with no terminator is
    // malformed rather than a key that happens to run to the end.
    const char* variant_text = reinterpret_cast<const char*>(data + kVariantOffset);
    usize length = 0;
    while (length < kVariantBytes && variant_text[length] != '\0') {
        ++length;
    }
    if (length >= kVariantBytes) {
        return fail(ErrorCode::InvalidArgument, "the variant key in the header is unterminated");
    }
    const Expected<VariantKey, Error> variant =
        VariantKey::parse(std::string_view(variant_text, length));
    if (!variant) {
        return make_unexpected(variant.error());
    }
    header.variant = variant.value();
    return header;
}

Expected<Span<const u8>, Error> read_cooked_payload(const u8* data, usize size,
                                                    bool verify_hash) noexcept {
    const Expected<CookedAssetHeader, Error> header = read_cooked_header(data, size);
    if (!header) {
        return make_unexpected(header.error());
    }
    if (kCookedHeaderBytes + header->payload_size > size) {
        return fail(ErrorCode::OutOfRange,
                    "the cooked asset declares more payload than the data holds");
    }
    const Span<const u8> payload(data + kCookedHeaderBytes,
                                 static_cast<usize>(header->payload_size));
    if (!verify_hash) {
        return payload;
    }

    ContentHasher hasher;
    if (!payload.empty()) {
        hasher.update(payload.data(), payload.size());
    }
    if (hasher.finish() != header->content) {
        return fail(ErrorCode::Io, "the cooked asset's payload does not match its recorded hash");
    }
    return payload;
}

Status check_cooked_variant(const CookedAssetHeader& header, VariantKey wanted) noexcept {
    if (header.variant == wanted) {
        return ok();
    }
    return fail(ErrorCode::InvalidArgument,
                "the cooked asset was produced for a different platform or feature variant");
}

}  // namespace cy::assets
