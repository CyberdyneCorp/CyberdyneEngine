// SPDX-License-Identifier: MIT
#include "secure_wire.h"

#include <cstring>

namespace cy::net::wire {

void put_u16(u8* out, u16 value) noexcept {
    out[0] = static_cast<u8>(value);
    out[1] = static_cast<u8>(value >> 8);
}

void put_u32(u8* out, u32 value) noexcept {
    for (u32 index = 0; index < 4; ++index) {
        out[index] = static_cast<u8>(value >> (index * 8));
    }
}

void put_u64(u8* out, u64 value) noexcept {
    for (u32 index = 0; index < 8; ++index) {
        out[index] = static_cast<u8>(value >> (index * 8));
    }
}

u16 get_u16(const u8* in) noexcept {
    return static_cast<u16>(in[0] | (in[1] << 8));
}

u32 get_u32(const u8* in) noexcept {
    u32 value = 0;
    for (u32 index = 0; index < 4; ++index) {
        value |= static_cast<u32>(in[index]) << (index * 8);
    }
    return value;
}

u64 get_u64(const u8* in) noexcept {
    u64 value = 0;
    for (u32 index = 0; index < 8; ++index) {
        value |= static_cast<u64>(in[index]) << (index * 8);
    }
    return value;
}

void packet_nonce(u64 counter, Direction direction, u8 (&nonce)[crypto::kNonceBytes]) noexcept {
    std::memset(nonce, 0, sizeof(nonce));
    put_u64(nonce, counter);
    nonce[8] = static_cast<u8>(direction);
}

namespace {

/// Domain separation: every derivation hashes its own label first, so no two of them can ever be
/// made to produce the same output from the same inputs.
constexpr char kCookieLabel[] = "cy.net cookie v1";
constexpr char kEphemeralLabel[] = "cy.net server ephemeral v1";
constexpr char kChallengeLabel[] = "cy.net challenge v1";
constexpr char kSessionLabel[] = "cy.net session v1";

[[nodiscard]] crypto::Bytes text(const char* label) noexcept {
    return {reinterpret_cast<const u8*>(label), std::strlen(label)};
}

[[nodiscard]] crypto::Bytes bytes_of(const crypto::Key& key) noexcept {
    return {key.bytes, crypto::kKeyBytes};
}

struct Encoded {
    u8 bytes[10] = {};
};

[[nodiscard]] Encoded encode_address(const SocketAddress& address) noexcept {
    Encoded out;
    put_u64(out.bytes, address.host);
    put_u16(out.bytes + 8, address.port);
    return out;
}

[[nodiscard]] Encoded encode_u64(u64 value) noexcept {
    Encoded out;
    put_u64(out.bytes, value);
    return out;
}

}  // namespace

Cookie make_cookie(const crypto::Key& secret, u64 protocol, u64 issued_s,
                   const crypto::Key& client_ephemeral, const SocketAddress& address) noexcept {
    const Encoded where = encode_address(address);
    const Encoded when = encode_u64(issued_s);
    const Encoded which = encode_u64(protocol);
    const crypto::Bytes parts[] = {
        text(kCookieLabel),         {which.bytes, 8},  {when.bytes, 8},
        bytes_of(client_ephemeral), {where.bytes, 10},
    };
    Cookie cookie;
    cookie.issued_s = issued_s;
    crypto::hash(cookie.tag, sizeof(cookie.tag), &secret, parts);
    return cookie;
}

crypto::Key server_ephemeral(const crypto::Key& secret, u64 issued_s,
                             const crypto::Key& client_ephemeral,
                             const SocketAddress& address) noexcept {
    const Encoded where = encode_address(address);
    const Encoded when = encode_u64(issued_s);
    const crypto::Bytes parts[] = {
        text(kEphemeralLabel),
        {when.bytes, 8},
        bytes_of(client_ephemeral),
        {where.bytes, 10},
    };
    crypto::Key out;
    crypto::hash(out.bytes, crypto::kKeyBytes, &secret, parts);
    return out;
}

crypto::Key challenge_key(const crypto::Key& static_shared, u64 protocol,
                          const crypto::Key& client_ephemeral,
                          const crypto::Key& server_ephemeral_public,
                          const crypto::Key& server_static_public) noexcept {
    const Encoded which = encode_u64(protocol);
    const crypto::Bytes parts[] = {
        text(kChallengeLabel),          {which.bytes, 8},
        bytes_of(client_ephemeral),     bytes_of(server_ephemeral_public),
        bytes_of(server_static_public),
    };
    crypto::Key out;
    crypto::hash(out.bytes, crypto::kKeyBytes, &static_shared, parts);
    return out;
}

SessionKeys session_keys(const crypto::Key& static_shared, const crypto::Key& ephemeral_shared,
                         u64 protocol, const crypto::Key& client_ephemeral,
                         const crypto::Key& server_ephemeral_public,
                         const crypto::Key& server_static_public) noexcept {
    const Encoded which = encode_u64(protocol);
    const crypto::Bytes parts[] = {
        text(kSessionLabel),
        bytes_of(static_shared),
        bytes_of(ephemeral_shared),
        {which.bytes, 8},
        bytes_of(client_ephemeral),
        bytes_of(server_ephemeral_public),
        bytes_of(server_static_public),
    };
    u8 derived[64];
    crypto::hash(derived, sizeof(derived), nullptr, parts);
    SessionKeys keys;
    std::memcpy(keys.client_to_server.bytes, derived, crypto::kKeyBytes);
    std::memcpy(keys.server_to_client.bytes, derived + crypto::kKeyBytes, crypto::kKeyBytes);
    crypto::wipe(derived, sizeof(derived));
    return keys;
}

// --- HELLO ---------------------------------------------------------------------------------------

void write_hello(u64 protocol, const crypto::Key& client_ephemeral,
                 u8 (&out)[kHelloBytes]) noexcept {
    std::memset(out, 0, sizeof(out));
    out[0] = static_cast<u8>(PacketType::Hello);
    put_u64(out + 1, protocol);
    std::memcpy(out + 9, client_ephemeral.bytes, crypto::kKeyBytes);
}

bool read_hello(Span<const u8> bytes, u64 protocol, crypto::Key& client_ephemeral) noexcept {
    if (bytes.size() != kHelloBytes || get_u64(bytes.data() + 1) != protocol) {
        return false;
    }
    std::memcpy(client_ephemeral.bytes, bytes.data() + 9, crypto::kKeyBytes);
    return true;
}

// --- CHALLENGE -----------------------------------------------------------------------------------

namespace {

constexpr u32 kChallengeTagOffset = kChallengeBytes - crypto::kMacBytes;

void lay_out_challenge(u64 protocol, const Challenge& challenge, u8* out) noexcept {
    out[0] = static_cast<u8>(PacketType::Challenge);
    put_u64(out + 1, protocol);
    std::memcpy(out + 9, challenge.server_ephemeral_public.bytes, crypto::kKeyBytes);
    put_u64(out + 41, challenge.cookie.issued_s);
    std::memcpy(out + 49, challenge.cookie.tag, kCookieTagBytes);
    std::memcpy(out + 65, challenge.nonce, crypto::kNonceBytes);
}

}  // namespace

void write_challenge(u64 protocol, const Challenge& challenge, const crypto::Key& key,
                     u8 (&out)[kChallengeBytes]) noexcept {
    lay_out_challenge(protocol, challenge, out);
    u8 mac[crypto::kMacBytes];
    // No plaintext: the challenge's contents are public, and what the tag proves is that whoever
    // made it could compute `key` — that is, holds the server's static secret.
    crypto::seal(key, challenge.nonce, Span<const u8>(out, kChallengeTagOffset), {}, nullptr, mac);
    std::memcpy(out + kChallengeTagOffset, mac, sizeof(mac));
}

bool peek_challenge(Span<const u8> bytes, u64 protocol, Challenge& out) noexcept {
    if (bytes.size() != kChallengeBytes || get_u64(bytes.data() + 1) != protocol) {
        return false;
    }
    std::memcpy(out.server_ephemeral_public.bytes, bytes.data() + 9, crypto::kKeyBytes);
    out.cookie.issued_s = get_u64(bytes.data() + 41);
    std::memcpy(out.cookie.tag, bytes.data() + 49, kCookieTagBytes);
    std::memcpy(out.nonce, bytes.data() + 65, crypto::kNonceBytes);
    return true;
}

bool read_challenge(Span<const u8> bytes, u64 protocol, const crypto::Key& key,
                    Challenge& out) noexcept {
    if (!peek_challenge(bytes, protocol, out)) {
        return false;
    }
    u8 mac[crypto::kMacBytes];
    std::memcpy(mac, bytes.data() + kChallengeTagOffset, sizeof(mac));
    return crypto::open(key, out.nonce, bytes.first(kChallengeTagOffset), {}, mac, nullptr);
}

// --- RESPONSE ------------------------------------------------------------------------------------

Status write_response(u64 protocol, const ResponseHeader& header,
                      const crypto::Key& client_to_server, Span<const u8> credential,
                      Array<u8>& out) noexcept {
    const usize plain_size = 2 + credential.size();
    if (Status sized = out.resize(kResponseHeaderBytes + plain_size + crypto::kMacBytes); !sized) {
        return sized;
    }
    u8* bytes = out.data();
    bytes[0] = static_cast<u8>(PacketType::Response);
    put_u64(bytes + 1, protocol);
    std::memcpy(bytes + 9, header.client_ephemeral.bytes, crypto::kKeyBytes);
    put_u64(bytes + 41, header.cookie.issued_s);
    std::memcpy(bytes + 49, header.cookie.tag, kCookieTagBytes);

    Array<u8> plain(out.allocator());
    if (Status sized = plain.resize(plain_size); !sized) {
        return sized;
    }
    put_u16(plain.data(), static_cast<u16>(credential.size()));
    if (!credential.empty()) {
        std::memcpy(plain.data() + 2, credential.data(), credential.size());
    }
    u8 nonce[crypto::kNonceBytes];
    packet_nonce(0, Direction::ClientToServer, nonce);
    u8 mac[crypto::kMacBytes];
    crypto::seal(client_to_server, nonce, Span<const u8>(bytes, kResponseHeaderBytes), plain.span(),
                 bytes + kResponseHeaderBytes, mac);
    std::memcpy(bytes + kResponseHeaderBytes + plain_size, mac, sizeof(mac));
    crypto::wipe(plain.data(), plain.size());
    return ok();
}

bool read_response_header(Span<const u8> bytes, u64 protocol, ResponseHeader& out) noexcept {
    if (bytes.size() < kResponseHeaderBytes + 2 + crypto::kMacBytes ||
        get_u64(bytes.data() + 1) != protocol) {
        return false;
    }
    std::memcpy(out.client_ephemeral.bytes, bytes.data() + 9, crypto::kKeyBytes);
    out.cookie.issued_s = get_u64(bytes.data() + 41);
    std::memcpy(out.cookie.tag, bytes.data() + 49, kCookieTagBytes);
    return true;
}

bool open_response(Span<const u8> bytes, const crypto::Key& client_to_server,
                   Array<u8>& out) noexcept {
    const usize cipher_size = bytes.size() - kResponseHeaderBytes - crypto::kMacBytes;
    Array<u8> plain(out.allocator());
    if (!plain.resize(cipher_size)) {
        return false;
    }
    u8 nonce[crypto::kNonceBytes];
    packet_nonce(0, Direction::ClientToServer, nonce);
    u8 mac[crypto::kMacBytes];
    std::memcpy(mac, bytes.data() + kResponseHeaderBytes + cipher_size, sizeof(mac));
    if (!crypto::open(client_to_server, nonce, bytes.first(kResponseHeaderBytes),
                      bytes.subspan(kResponseHeaderBytes, cipher_size), mac, plain.data())) {
        return false;
    }
    const u16 size = get_u16(plain.data());
    if (static_cast<usize>(size) + 2 != cipher_size) {
        return false;
    }
    out.clear();
    return static_cast<bool>(out.append(plain.span().subspan(2)));
}

// --- DATA ----------------------------------------------------------------------------------------

Status seal_data(const DataHeader& header, Direction direction, const crypto::Key& key,
                 Span<const u8> plain, Array<u8>& out) noexcept {
    if (Status sized = out.resize(kDataOverhead + plain.size()); !sized) {
        return sized;
    }
    u8* bytes = out.data();
    bytes[0] = static_cast<u8>(PacketType::Data);
    put_u32(bytes + 1, header.connection);
    put_u64(bytes + 5, header.counter);
    u8 nonce[crypto::kNonceBytes];
    packet_nonce(header.counter, direction, nonce);
    u8 mac[crypto::kMacBytes];
    crypto::seal(key, nonce, Span<const u8>(bytes, kDataHeaderBytes), plain,
                 bytes + kDataHeaderBytes, mac);
    std::memcpy(bytes + kDataHeaderBytes + plain.size(), mac, sizeof(mac));
    return ok();
}

bool read_data_header(Span<const u8> bytes, DataHeader& out) noexcept {
    if (bytes.size() < kDataOverhead) {
        return false;
    }
    out.connection = get_u32(bytes.data() + 1);
    out.counter = get_u64(bytes.data() + 5);
    return true;
}

bool open_data(Span<const u8> bytes, Direction direction, const crypto::Key& key,
               Array<u8>& plain) noexcept {
    const usize cipher_size = bytes.size() - kDataOverhead;
    if (!plain.resize(cipher_size)) {
        return false;
    }
    u8 nonce[crypto::kNonceBytes];
    packet_nonce(get_u64(bytes.data() + 5), direction, nonce);
    u8 mac[crypto::kMacBytes];
    std::memcpy(mac, bytes.data() + kDataHeaderBytes + cipher_size, sizeof(mac));
    return crypto::open(key, nonce, bytes.first(kDataHeaderBytes),
                        bytes.subspan(kDataHeaderBytes, cipher_size), mac, plain.data());
}

Status append_frame(FrameKind kind, Span<const u8> body, Array<u8>& out) noexcept {
    u8 header[kFrameHeaderBytes];
    header[0] = static_cast<u8>(kind);
    put_u16(header + 1, static_cast<u16>(body.size()));
    if (Status appended = out.append(Span<const u8>(header, kFrameHeaderBytes)); !appended) {
        return appended;
    }
    return out.append(body);
}

}  // namespace cy::net::wire
