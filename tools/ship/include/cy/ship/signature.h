// SPDX-License-Identifier: MIT
#ifndef CY_SHIP_SIGNATURE_H
#define CY_SHIP_SIGNATURE_H
// Ed25519 signatures over a shipping manifest. Issue #87.
//
// `build-and-packaging` — "Manifest integrity and content protection": a shipping content manifest
// SHALL be signed and verified, with established implementations rather than bespoke ones. The
// scheme is RFC 8032 Ed25519 from Monocypher (deps/manifest.toml); src/signature.cpp is the only
// translation unit that names a `crypto_` symbol, so the library could be swapped without a caller
// noticing.
//
// WHAT A SIGNATURE PROVES, AND WHAT IT DOES NOT. A valid signature proves that whoever holds the
// secret key wrote these manifest bytes, and the manifest pins every file of the artefact by its
// BLAKE3 digest — so a chunk, a library or a document changed after signing is refused at startup.
// It does NOT stop a person who replaces the executable itself: the check runs inside it. That is
// what the platform's own code signing (macOS codesign, Windows Authenticode) is for, and it is not
// anti-tamper or anti-cheat, which the specification says content protection must not be called.

#include <cy/core/base/types.h>

#include <string>
#include <string_view>

namespace cy::ship {

struct PublicKey {
    static constexpr usize kBytes = 32;
    u8 bytes[kBytes] = {};
};

/// The 32-byte seed RFC 8032 calls the private key. The expanded key is derived from it per use.
struct SecretKey {
    static constexpr usize kBytes = 32;
    u8 seed[kBytes] = {};
};

struct Signature {
    static constexpr usize kBytes = 64;
    u8 bytes[kBytes] = {};
};

/// The public key a secret seed implies.
[[nodiscard]] PublicKey public_key(const SecretKey& secret) noexcept;

[[nodiscard]] Signature sign(const SecretKey& secret, std::string_view message) noexcept;

/// True only when `signature` is `key`'s over exactly `message`.
[[nodiscard]] bool verify(const PublicKey& key, std::string_view message,
                          const Signature& signature) noexcept;

/// THE DEVELOPMENT KEY, AND WHY IT IS NOT A SECRET. Its seed is BLAKE3 of a sentence printed in
/// signature.cpp, so anyone can sign with it. It exists so that a checkout packages and launches a
/// game with no key handling at all; a build configured with `-D CY_SHIP_PUBLIC_KEY=<hex>` trusts
/// that key instead and refuses content the development key signed. docs/guides/shipping.md.
[[nodiscard]] SecretKey development_secret_key() noexcept;

/// The key this build was configured to trust: CY_SHIP_PUBLIC_KEY, or the development key.
[[nodiscard]] PublicKey trusted_key() noexcept;
[[nodiscard]] bool trusts_development_key() noexcept;

/// A key's identity in a manifest and a diagnostic: the first eight bytes, as sixteen hex digits.
[[nodiscard]] std::string key_id(const PublicKey& key);

/// Lowercase hex, and its inverse. `parse_hex` fails unless `text` is exactly `2 * size` digits.
[[nodiscard]] std::string to_hex(const u8* bytes, usize size);
[[nodiscard]] bool parse_hex(std::string_view text, u8* out, usize size) noexcept;

}  // namespace cy::ship

#endif  // CY_SHIP_SIGNATURE_H
