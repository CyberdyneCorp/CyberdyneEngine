#ifndef CY_CORE_ASSETS_HASH_H
#define CY_CORE_ASSETS_HASH_H
// `ContentHash` — the engine's content-addressing digest. Tasks 3.3.1 and 3.3.3.
//
// `core-assets-and-io` — "Compression and cryptography": a cooked asset's BLAKE3 content hash is
// recorded for cache validation and patch diffing, and cryptographic primitives are backed by a
// vetted third-party implementation rather than hand-rolled. The implementation is the pinned
// blake3 dependency; NOTHING OF IT APPEARS HERE. `ContentHasher` holds opaque storage whose size is
// checked against the real one in hash.cpp, so replacing the codec is a change to that file.
//
// WHAT THIS FILE IS NOT. `core/crypto` in the specification also names SHA-256, HMAC, AES-GCM and a
// CSPRNG. None of them is here: nothing at M1 encrypts, signs or authenticates, and a primitive
// with no consumer is a primitive nobody has tested against a real use. The seam is the shape of
// this file — an engine-owned digest type over a vetted implementation — and package encryption
// (`PackageFlags::EncryptedPayload`) is a declared flag that mounting refuses, so the day the key
// management exists there is one place to fill in.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <string_view>

namespace cy::assets {

/// A 256-bit content digest. Content-addressed, so equality of two hashes is treated as equality of
/// two payloads: BLAKE3 at 256 bits, which is what the whole content-addressing scheme rests on.
struct ContentHash {
    static constexpr usize kByteLength = 32;
    static constexpr usize kTextLength = 64;

    u8 bytes[kByteLength] = {};

    [[nodiscard]] bool is_zero() const noexcept;

    /// The canonical text form: 64 lowercase hex digits. Writes the terminator too.
    void format(char (&out)[kTextLength + 1]) const noexcept;

    /// Parse the canonical form. Rejects anything that is not exactly 64 hex digits.
    [[nodiscard]] static Expected<ContentHash, Error> parse(std::string_view text) noexcept;

    friend bool operator==(const ContentHash& a, const ContentHash& b) noexcept;
    friend bool operator!=(const ContentHash& a, const ContentHash& b) noexcept {
        return !(a == b);
    }
    /// A total order over the bytes, so a chunk table sorts identically everywhere.
    friend bool operator<(const ContentHash& a, const ContentHash& b) noexcept;
};

/// Digest a whole buffer.
[[nodiscard]] ContentHash content_hash(const void* data, usize size) noexcept;

/// Digest a payload arriving in pieces — a file read a block at a time, a package written entry by
/// entry. `finish()` may be called once; calling it again is a programmer error.
class ContentHasher {
public:
    ContentHasher() noexcept;

    ContentHasher(const ContentHasher&) = delete;
    ContentHasher& operator=(const ContentHasher&) = delete;

    void update(const void* data, usize size) noexcept;
    [[nodiscard]] ContentHash finish() noexcept;
    /// Start over, so one hasher can digest a sequence of payloads without being rebuilt.
    void reset() noexcept;

private:
    // Opaque storage for the implementation's state. The size and alignment are asserted against
    // the real type in hash.cpp, so this cannot drift silently when the dependency is updated.
    alignas(64) u8 state_[1936] = {};
    bool finished_ = false;
};

/// Hash for the standard associative containers, keyed on a content hash.
struct ContentHashHash {
    [[nodiscard]] usize operator()(const ContentHash& hash) const noexcept;
};

}  // namespace cy::assets

#endif  // CY_CORE_ASSETS_HASH_H
