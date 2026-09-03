// AssetId's text form. Task 1.3.2.

#include <cy/core/values/asset_id.h>

namespace cy {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

/// The value of one hex digit, or 16 for anything else — one branch at the call site rather than
/// three ranges repeated per character.
u32 hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return static_cast<u32>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<u32>(c - 'a') + 10u;
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<u32>(c - 'A') + 10u;
    }
    return 16u;
}

void write_hex64(u64 value, char* out) noexcept {
    for (u32 i = 0; i < 16; ++i) {
        const u32 nibble = static_cast<u32>((value >> ((15u - i) * 4u)) & 0xfu);
        out[i] = kHexDigits[nibble];
    }
}

Expected<u64, Error> read_hex64(std::string_view text) noexcept {
    u64 value = 0;
    for (const char c : text) {
        const u32 digit = hex_value(c);
        if (digit > 15u) {
            return fail(ErrorCode::InvalidArgument,
                        "asset id contains a non-hexadecimal character");
        }
        value = (value << 4) | digit;
    }
    return value;
}

}  // namespace

usize AssetId::format(char (&out)[kTextLength + 1]) const noexcept {
    write_hex64(high_, out);
    write_hex64(low_, out + 16);
    out[kTextLength] = '\0';
    return kTextLength;
}

Expected<AssetId, Error> AssetId::parse(std::string_view text) noexcept {
    if (text.size() != kTextLength) {
        return fail(ErrorCode::InvalidArgument, "an asset id is exactly 32 hexadecimal digits");
    }
    const Expected<u64, Error> high = read_hex64(text.substr(0, 16));
    if (!high) {
        return make_unexpected(high.error());
    }
    const Expected<u64, Error> low = read_hex64(text.substr(16, 16));
    if (!low) {
        return make_unexpected(low.error());
    }
    return AssetId(*high, *low);
}

}  // namespace cy
