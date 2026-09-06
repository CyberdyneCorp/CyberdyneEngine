#include <cy/core/assets/derivation.h>

#include <cstring>

namespace cy::assets {
namespace {

/// Little-endian, because a shared cache is populated by one machine and read by another and
/// nothing promises they agree about byte order.
void write_u64_le(u8* out, u64 value) noexcept {
    for (u32 index = 0; index < 8; ++index) {
        out[index] = static_cast<u8>((value >> (index * 8U)) & 0xFFU);
    }
}

}  // namespace

const char* derived_kind_name(DerivedKind kind) noexcept {
    switch (kind) {
        case DerivedKind::Unknown:
            return "unknown";
        case DerivedKind::Import:
            return "import";
        case DerivedKind::Shader:
            return "shader";
        case DerivedKind::MaterialProgram:
            return "material-program";
        case DerivedKind::Page:
            return "page";
        case DerivedKind::Metadata:
            return "metadata";
    }
    return "unknown";
}

Expected<DerivedKind, Error> derived_kind_from_name(std::string_view name) noexcept {
    constexpr DerivedKind kAll[] = {DerivedKind::Unknown, DerivedKind::Import,
                                    DerivedKind::Shader,  DerivedKind::MaterialProgram,
                                    DerivedKind::Page,    DerivedKind::Metadata};
    for (const DerivedKind kind : kAll) {
        if (name == derived_kind_name(kind)) {
            return kind;
        }
    }
    return fail(ErrorCode::InvalidArgument, "not a derived-data kind this build defines");
}

Expected<DerivationKey, Error> DerivationKey::parse(std::string_view text) noexcept {
    Expected<ContentHash, Error> digest = ContentHash::parse(text);
    if (!digest) {
        return make_unexpected(digest.error());
    }
    return DerivationKey{digest.value()};
}

// --- The builder ---------------------------------------------------------------------------------

void DerivationKeyBuilder::frame(Tag tag, std::string_view name, const void* value,
                                 usize value_size) noexcept {
    // The framing that makes the encoding prefix-free: tag, name length, name, value length, value.
    // Without the two lengths, ("ab", "c") and ("a", "bc") hash identically and the cache serves
    // one computation's artefact for another's. See the header.
    const u8 tag_byte = static_cast<u8>(tag);
    hasher_.update(&tag_byte, 1);

    u8 length[8] = {};
    write_u64_le(length, name.size());
    hasher_.update(length, sizeof(length));
    if (!name.empty()) {
        hasher_.update(name.data(), name.size());
    }

    write_u64_le(length, value_size);
    hasher_.update(length, sizeof(length));
    if (value_size != 0 && value != nullptr) {
        hasher_.update(value, value_size);
    }

    ++contributions_;
}

DerivationKeyBuilder& DerivationKeyBuilder::producer(DerivedKind kind, std::string_view name,
                                                     u32 version) noexcept {
    CY_ASSERT_MSG(!finished_, "a derivation key cannot be extended after it is finished");
    // Kind and version travel together with the name because all three answer "which code produced
    // this". A version bump and a kind change must both invalidate, and they do.
    u8 payload[16] = {};
    write_u64_le(payload, static_cast<u64>(kind));
    write_u64_le(payload + 8, version);
    frame(Tag::Producer, name, payload, sizeof(payload));
    has_producer_ = true;
    return *this;
}

DerivationKeyBuilder& DerivationKeyBuilder::source(std::string_view name,
                                                   const ContentHash& hash) noexcept {
    CY_ASSERT_MSG(!finished_, "a derivation key cannot be extended after it is finished");
    frame(Tag::Source, name, hash.bytes, ContentHash::kByteLength);
    return *this;
}

DerivationKeyBuilder& DerivationKeyBuilder::text(std::string_view name,
                                                 std::string_view value) noexcept {
    CY_ASSERT_MSG(!finished_, "a derivation key cannot be extended after it is finished");
    frame(Tag::Text, name, value.data(), value.size());
    return *this;
}

DerivationKeyBuilder& DerivationKeyBuilder::number(std::string_view name, u64 value) noexcept {
    CY_ASSERT_MSG(!finished_, "a derivation key cannot be extended after it is finished");
    u8 encoded[8] = {};
    write_u64_le(encoded, value);
    frame(Tag::Number, name, encoded, sizeof(encoded));
    return *this;
}

DerivationKeyBuilder& DerivationKeyBuilder::flag(std::string_view name, bool value) noexcept {
    CY_ASSERT_MSG(!finished_, "a derivation key cannot be extended after it is finished");
    const u8 encoded = value ? 1U : 0U;
    frame(Tag::Flag, name, &encoded, 1);
    return *this;
}

DerivationKeyBuilder& DerivationKeyBuilder::bytes(std::string_view name, const void* data,
                                                  usize size) noexcept {
    CY_ASSERT_MSG(!finished_, "a derivation key cannot be extended after it is finished");
    frame(Tag::Bytes, name, data, size);
    return *this;
}

Expected<DerivationKey, Error> DerivationKeyBuilder::finish() noexcept {
    CY_ASSERT_MSG(!finished_, "a derivation key is finished once");
    if (!has_producer_) {
        return fail(ErrorCode::InvalidArgument,
                    "a derivation key must declare its producer and version");
    }
    finished_ = true;
    return DerivationKey{hasher_.finish()};
}

}  // namespace cy::assets
