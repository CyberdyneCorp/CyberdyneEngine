// SPDX-License-Identifier: MIT
// The one translation unit in cy::net that names a Monocypher symbol. See crypto.h.

#include <cy/networking/crypto.h>

#include <monocypher.h>

#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
// windows.h must precede bcrypt.h, which uses its types.
#    include <bcrypt.h>
#    pragma comment(lib, "bcrypt.lib")
#elif defined(__APPLE__)
#    include <stdlib.h>
#elif defined(__linux__)
#    include <sys/random.h>

#    include <cerrno>
#endif

namespace cy::net::crypto {

bool operator==(const Key& a, const Key& b) noexcept {
    return crypto_verify32(a.bytes, b.bytes) == 0;
}

namespace {

void fill_from_system(void* /*user*/, u8* out, usize size) noexcept {
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, out, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) !=
        0) {
        std::abort();  // See the Linux branch: no entropy is not recoverable.
    }
#elif defined(__APPLE__)
    arc4random_buf(out, size);
#elif defined(__linux__)
    usize filled = 0;
    while (filled < size) {
        const ssize_t got = getrandom(out + filled, size - filled, 0);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            // No entropy is not an error a caller can recover from: a key made of whatever the
            // buffer held is a key an attacker may know. Stop rather than continue unprotected.
            std::abort();
        }
        filled += static_cast<usize>(got);
    }
#else
#    error "cy::net::crypto needs a cryptographic random source on this platform"
#endif
}

}  // namespace

EntropySource system_entropy() noexcept {
    EntropySource source;
    source.fill = &fill_from_system;
    source.cryptographic = true;
    return source;
}

EntropySource SeededEntropy::source() noexcept {
    EntropySource source;
    source.fill = &SeededEntropy::fill;
    source.user = this;
    source.cryptographic = false;
    return source;
}

void SeededEntropy::fill(void* user, u8* out, usize size) noexcept {
    auto* self = static_cast<SeededEntropy*>(user);
    while (size > 0) {
        u8 block[64];
        u8 input[16];
        for (u32 index = 0; index < 8; ++index) {
            input[index] = static_cast<u8>(self->seed_ >> (index * 8));
            input[8 + index] = static_cast<u8>(self->counter_ >> (index * 8));
        }
        ++self->counter_;
        crypto_blake2b(block, sizeof(block), input, sizeof(input));
        const usize take = size < sizeof(block) ? size : sizeof(block);
        std::memcpy(out, block, take);
        out += take;
        size -= take;
    }
}

KeyPair generate_key_pair(const EntropySource& entropy) noexcept {
    KeyPair pair;
    entropy(pair.secret.bytes, kKeyBytes);
    crypto_x25519_public_key(pair.public_key.bytes, pair.secret.bytes);
    return pair;
}

Key public_key_of(const Key& secret) noexcept {
    Key out;
    crypto_x25519_public_key(out.bytes, secret.bytes);
    return out;
}

bool key_exchange(const Key& secret, const Key& their_public, Key& shared) noexcept {
    crypto_x25519(shared.bytes, secret.bytes, their_public.bytes);
    const Key zero;
    return !(shared == zero);
}

void hash(u8* out, usize out_size, const Key* key, Span<const Bytes> parts) noexcept {
    crypto_blake2b_ctx context;
    if (key != nullptr) {
        crypto_blake2b_keyed_init(&context, out_size, key->bytes, kKeyBytes);
    } else {
        crypto_blake2b_init(&context, out_size);
    }
    for (const Bytes& part : parts) {
        crypto_blake2b_update(&context, part.data, part.size);
    }
    crypto_blake2b_final(&context, out);
}

void seal(const Key& key, const u8 (&nonce)[kNonceBytes], Span<const u8> associated,
          Span<const u8> plain, u8* cipher, u8 (&mac)[kMacBytes]) noexcept {
    crypto_aead_lock(cipher, mac, key.bytes, nonce, associated.data(), associated.size(),
                     plain.data(), plain.size());
}

bool open(const Key& key, const u8 (&nonce)[kNonceBytes], Span<const u8> associated,
          Span<const u8> cipher, const u8 (&mac)[kMacBytes], u8* plain) noexcept {
    return crypto_aead_unlock(plain, mac, key.bytes, nonce, associated.data(), associated.size(),
                              cipher.data(), cipher.size()) == 0;
}

bool equal_tags(const u8* a, const u8* b) noexcept {
    return crypto_verify16(a, b) == 0;
}

void wipe(void* secret, usize size) noexcept {
    crypto_wipe(secret, size);
}

}  // namespace cy::net::crypto
