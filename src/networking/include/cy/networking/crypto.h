// SPDX-License-Identifier: MIT
#pragma once
// The cryptographic primitives the secure transport is built from, and nothing else. Issue #82.
//
// ================================================================================================
// NO CIPHER IS WRITTEN HERE
// ================================================================================================
//
// `networking-and-replication` — "Security posture": transports SHALL support encryption and
// authentication, and `build-and-packaging` says established implementations SHALL be used. Every
// function below is a thin, typed wrapper over **Monocypher 4.0.2** (`deps/manifest.toml`, audited
// by Cure53 in 2020): X25519 for key agreement, XChaCha20-Poly1305 for authenticated encryption and
// BLAKE2b for key derivation and keyed message authentication. `src/crypto.cpp` is the one
// translation unit in this module that names a `crypto_` symbol, the same discipline
// `tools/ship/src/signature.cpp` keeps, so the library never leaks into a header.
//
// ================================================================================================
// ENTROPY IS INJECTED, AND THE DEFAULT IS THE OPERATING SYSTEM'S
// ================================================================================================
//
// Every key, nonce and cookie secret comes from an `EntropySource`. The default is the platform's
// cryptographic generator (`getrandom`, `arc4random_buf`, `BCryptGenRandom`). A test may pass a
// seeded source so that a session under the simulated network is reproducible packet for packet —
// and `SecureTransport::security()` then stops reporting itself encrypted for a shipping
// deployment, because a key a test can predict protects nothing.

#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::net::crypto {

/// An X25519 key, a ChaCha20 key and a BLAKE2b key are all 32 bytes.
inline constexpr usize kKeyBytes = 32;
/// A Poly1305 tag.
inline constexpr usize kMacBytes = 16;
/// An XChaCha20 nonce: wide enough that a random one never repeats.
inline constexpr usize kNonceBytes = 24;

/// 32 bytes: an X25519 key, a symmetric key, or a derived secret.
struct Key {
    u8 bytes[kKeyBytes] = {};

    friend bool operator==(const Key& a, const Key& b) noexcept;
};

/// An X25519 key pair. The secret half is wiped by `wipe()`, which a holder calls on teardown.
struct KeyPair {
    Key secret;
    Key public_key;
};

/// Where random bytes come from. A function pointer and its argument, so a test can substitute a
/// seeded generator without this header naming one.
struct EntropySource {
    void (*fill)(void* user, u8* out, usize size) noexcept = nullptr;
    void* user = nullptr;
    /// True only for the operating system's generator. A seeded source is reproducible, which is
    /// what a test wants and what a shipping session must never have.
    bool cryptographic = false;

    void operator()(u8* out, usize size) const noexcept { fill(user, out, size); }
};

/// The operating system's cryptographically secure generator.
[[nodiscard]] EntropySource system_entropy() noexcept;

/// A reproducible generator for tests and tools: BLAKE2b in counter mode over `seed`. Never
/// cryptographic in the sense above — anybody holding the seed holds every key it produced.
class SeededEntropy {
public:
    explicit SeededEntropy(u64 seed) noexcept : seed_(seed) {}

    [[nodiscard]] EntropySource source() noexcept;

private:
    static void fill(void* user, u8* out, usize size) noexcept;

    u64 seed_;
    u64 counter_ = 0;
};

/// A fresh key pair from `entropy`.
[[nodiscard]] KeyPair generate_key_pair(const EntropySource& entropy) noexcept;

/// The public half of `secret`.
[[nodiscard]] Key public_key_of(const Key& secret) noexcept;

/// X25519. Returns false when the result is all zero, which is what a peer offering a low-order
/// point produces: a shared secret every attacker also knows.
[[nodiscard]] bool key_exchange(const Key& secret, const Key& their_public, Key& shared) noexcept;

/// One input to `hash()`: borrowed bytes.
struct Bytes {
    const u8* data = nullptr;
    usize size = 0;
};

/// BLAKE2b over the concatenation of `parts`, keyed when `key` is non-null. `out_size` is 1 to 64.
/// What derives every key and tags every cookie.
void hash(u8* out, usize out_size, const Key* key, Span<const Bytes> parts) noexcept;

/// XChaCha20-Poly1305: encrypt `plain` into `cipher` (same size) and write the tag to `mac`.
void seal(const Key& key, const u8 (&nonce)[kNonceBytes], Span<const u8> associated,
          Span<const u8> plain, u8* cipher, u8 (&mac)[kMacBytes]) noexcept;

/// The inverse. Returns false, and writes nothing to `plain`, when the tag does not authenticate
/// `cipher` and `associated` under `key` and `nonce` — a tampered packet, a forged one, or one
/// sealed under another key.
[[nodiscard]] bool open(const Key& key, const u8 (&nonce)[kNonceBytes], Span<const u8> associated,
                        Span<const u8> cipher, const u8 (&mac)[kMacBytes], u8* plain) noexcept;

/// Constant-time comparison of two 16-byte tags.
[[nodiscard]] bool equal_tags(const u8* a, const u8* b) noexcept;

/// Overwrite secret material so it does not outlive its use in memory.
void wipe(void* secret, usize size) noexcept;

}  // namespace cy::net::crypto
