// The text form of a persistent identity, and the enumerator spellings. Task 6.1.

#include <cy/save/identity.h>

namespace cy::save {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

/// The value of one hex digit, or 16 for anything else — one branch at the call site rather than
/// three ranges repeated per character.
u32 hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return static_cast<u32>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<u32>(c - 'a') + 10U;
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<u32>(c - 'A') + 10U;
    }
    return 16U;
}

void write_hex64(u64 value, char* out) noexcept {
    for (u32 index = 0; index < 16U; ++index) {
        const u32 nibble = static_cast<u32>((value >> ((15U - index) * 4U)) & 0xfU);
        out[index] = kHexDigits[nibble];
    }
}

Expected<u64, Error> read_hex64(std::string_view text) noexcept {
    u64 value = 0;
    for (const char digit_character : text) {
        const u32 digit = hex_value(digit_character);
        if (digit > 15U) {
            return fail(ErrorCode::InvalidArgument,
                        "a persistent id contains a non-hexadecimal character");
        }
        value = (value << 4U) | digit;
    }
    return value;
}

}  // namespace

usize PersistentId::format(char (&out)[kTextLength + 1]) const noexcept {
    write_hex64(high_, out);
    write_hex64(low_, out + 16);
    out[kTextLength] = '\0';
    return kTextLength;
}

Expected<PersistentId, Error> PersistentId::parse(std::string_view text) noexcept {
    if (text.size() != kTextLength) {
        return fail(ErrorCode::InvalidArgument, "a persistent id is exactly 32 hexadecimal digits");
    }
    const Expected<u64, Error> high = read_hex64(text.substr(0, 16));
    if (!high) {
        return make_unexpected(high.error());
    }
    const Expected<u64, Error> low = read_hex64(text.substr(16, 16));
    if (!low) {
        return make_unexpected(low.error());
    }
    return PersistentId(*high, *low);
}

const char* scope_name(Scope scope) noexcept {
    switch (scope) {
        case Scope::Profile:
            return "profile";
        case Scope::GameInstance:
            return "game-instance";
        case Scope::Campaign:
            return "campaign";
        case Scope::Session:
            return "session";
        case Scope::World:
            return "world";
        case Scope::Participant:
            return "participant";
        case Scope::Count:
            break;
    }
    return "unknown";
}

}  // namespace cy::save
