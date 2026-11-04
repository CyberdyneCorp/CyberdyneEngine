// SPDX-License-Identifier: MIT
// The one translation unit that names a Monocypher symbol. See signature.h.

#include <cy/core/assets/hash.h>
#include <cy/ship/signature.h>

#include <monocypher-ed25519.h>
#include <monocypher.h>

#include <cstring>

namespace cy::ship {

namespace {

/// The expanded 64-byte key Monocypher signs with, derived from the seed and wiped after use.
struct ExpandedKey {
    u8 secret[64] = {};
    u8 public_key[PublicKey::kBytes] = {};

    explicit ExpandedKey(const SecretKey& key) noexcept {
        u8 seed[SecretKey::kBytes];
        std::memcpy(seed, key.seed, sizeof(seed));
        // crypto_ed25519_key_pair wipes the seed it is handed, so it is handed a copy.
        crypto_ed25519_key_pair(secret, public_key, seed);
    }
    ~ExpandedKey() { crypto_wipe(secret, sizeof(secret)); }

    ExpandedKey(const ExpandedKey&) = delete;
    ExpandedKey& operator=(const ExpandedKey&) = delete;
};

[[nodiscard]] int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// PUBLISHED ON PURPOSE. The development key's seed is the BLAKE3 digest of this sentence, which
// makes the key reproducible on every machine and secret on none.
constexpr std::string_view kDevelopmentSentence =
    "CyberdyneEngine development signing key: published, so it proves nothing about who signed";

}  // namespace

PublicKey public_key(const SecretKey& secret) noexcept {
    const ExpandedKey expanded(secret);
    PublicKey key;
    std::memcpy(key.bytes, expanded.public_key, sizeof(key.bytes));
    return key;
}

Signature sign(const SecretKey& secret, std::string_view message) noexcept {
    const ExpandedKey expanded(secret);
    Signature signature;
    crypto_ed25519_sign(signature.bytes, expanded.secret,
                        reinterpret_cast<const u8*>(message.data()), message.size());
    return signature;
}

bool verify(const PublicKey& key, std::string_view message, const Signature& signature) noexcept {
    return crypto_ed25519_check(signature.bytes, key.bytes,
                                reinterpret_cast<const u8*>(message.data()), message.size()) == 0;
}

SecretKey development_secret_key() noexcept {
    const assets::ContentHash digest =
        assets::content_hash(kDevelopmentSentence.data(), kDevelopmentSentence.size());
    SecretKey key;
    static_assert(sizeof(key.seed) == assets::ContentHash::kByteLength);
    std::memcpy(key.seed, digest.bytes, sizeof(key.seed));
    return key;
}

PublicKey trusted_key() noexcept {
#if defined(CY_SHIP_PUBLIC_KEY)
    // tools/ship/CMakeLists.txt checked the configured value is 64 hex digits.
    PublicKey configured;
    if (parse_hex(CY_SHIP_PUBLIC_KEY, configured.bytes, sizeof(configured.bytes))) {
        return configured;
    }
#endif
    return public_key(development_secret_key());
}

bool trusts_development_key() noexcept {
    const PublicKey trusted = trusted_key();
    const PublicKey development = public_key(development_secret_key());
    return std::memcmp(trusted.bytes, development.bytes, sizeof(trusted.bytes)) == 0;
}

std::string key_id(const PublicKey& key) {
    return to_hex(key.bytes, 8);
}

std::string to_hex(const u8* bytes, usize size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text(size * 2, '0');
    for (usize index = 0; index < size; ++index) {
        text[index * 2] = kDigits[bytes[index] >> 4U];
        text[(index * 2) + 1] = kDigits[bytes[index] & 0x0FU];
    }
    return text;
}

bool parse_hex(std::string_view text, u8* out, usize size) noexcept {
    if (text.size() != size * 2) {
        return false;
    }
    for (usize index = 0; index < size; ++index) {
        const int high = hex_digit(text[index * 2]);
        const int low = hex_digit(text[(index * 2) + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[index] = static_cast<u8>((high << 4) | low);
    }
    return true;
}

}  // namespace cy::ship
