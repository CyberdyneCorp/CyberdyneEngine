// SPDX-License-Identifier: MIT
#pragma once
// The secure transport's wire formats and key schedule. Private to src/networking.
//
// Every multi-byte field is little-endian and written field by field, never as a struct, for the
// reason `reliability.h` gives for its own header. Each packet layout below is a function pair —
// write and read — so the layout is stated once and both directions agree by construction.

#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/networking/crypto.h>
#include <cy/networking/datagram_socket.h>

namespace cy::net::wire {

enum class PacketType : u8 {
    Hello = 1,
    Challenge = 2,
    Response = 3,
    Data = 4,
};

/// Plaintext frames inside a data packet.
enum class FrameKind : u8 {
    Datagram = 1,
    Ping = 2,
    Pong = 3,
    Disconnect = 4,
    Accept = 5,
    Deny = 6,
};

enum class Direction : u8 {
    ClientToServer = 0,
    ServerToClient = 1,
};

inline constexpr u32 kHelloBytes = 192;
inline constexpr u32 kCookieTagBytes = 16;
inline constexpr u32 kChallengeBytes =
    1 + 8 + 32 + 8 + kCookieTagBytes + crypto::kNonceBytes + crypto::kMacBytes;
inline constexpr u32 kResponseHeaderBytes = 1 + 8 + 32 + 8 + kCookieTagBytes;
inline constexpr u32 kDataHeaderBytes = 1 + 4 + 8;
inline constexpr u32 kDataOverhead = kDataHeaderBytes + crypto::kMacBytes;
inline constexpr u32 kFrameHeaderBytes = 3;
inline constexpr u32 kPingBytes = 4 + 8 + 8 + 8;
inline constexpr u32 kPongBytes = 4 + 8;

static_assert(kHelloBytes > kChallengeBytes,
              "HELLO must be larger than CHALLENGE, or a spoofed HELLO amplifies traffic");

void put_u16(u8* out, u16 value) noexcept;
void put_u32(u8* out, u32 value) noexcept;
void put_u64(u8* out, u64 value) noexcept;
[[nodiscard]] u16 get_u16(const u8* in) noexcept;
[[nodiscard]] u32 get_u32(const u8* in) noexcept;
[[nodiscard]] u64 get_u64(const u8* in) noexcept;

/// A packet counter as an XChaCha20 nonce, with the direction so the two never coincide.
void packet_nonce(u64 counter, Direction direction, u8 (&nonce)[crypto::kNonceBytes]) noexcept;

struct Cookie {
    u64 issued_s = 0;
    u8 tag[kCookieTagBytes] = {};
};

/// The stateless cookie: a keyed tag over who asked, with which key, and when.
[[nodiscard]] Cookie make_cookie(const crypto::Key& secret, u64 protocol, u64 issued_s,
                                 const crypto::Key& client_ephemeral,
                                 const SocketAddress& address) noexcept;

/// The server's ephemeral secret for one handshake, rebuilt from the cookie's inputs.
[[nodiscard]] crypto::Key server_ephemeral(const crypto::Key& secret, u64 issued_s,
                                           const crypto::Key& client_ephemeral,
                                           const SocketAddress& address) noexcept;

/// The key CHALLENGE is tagged under: from X25519(client ephemeral, server static).
[[nodiscard]] crypto::Key challenge_key(const crypto::Key& static_shared, u64 protocol,
                                        const crypto::Key& client_ephemeral,
                                        const crypto::Key& server_ephemeral_public,
                                        const crypto::Key& server_static_public) noexcept;

struct SessionKeys {
    crypto::Key client_to_server;
    crypto::Key server_to_client;
};

[[nodiscard]] SessionKeys session_keys(const crypto::Key& static_shared,
                                       const crypto::Key& ephemeral_shared, u64 protocol,
                                       const crypto::Key& client_ephemeral,
                                       const crypto::Key& server_ephemeral_public,
                                       const crypto::Key& server_static_public) noexcept;

// --- HELLO ---------------------------------------------------------------------------------------

void write_hello(u64 protocol, const crypto::Key& client_ephemeral,
                 u8 (&out)[kHelloBytes]) noexcept;
[[nodiscard]] bool read_hello(Span<const u8> bytes, u64 protocol,
                              crypto::Key& client_ephemeral) noexcept;

// --- CHALLENGE -----------------------------------------------------------------------------------

struct Challenge {
    crypto::Key server_ephemeral_public;
    Cookie cookie;
    u8 nonce[crypto::kNonceBytes] = {};
};

void write_challenge(u64 protocol, const Challenge& challenge, const crypto::Key& key,
                     u8 (&out)[kChallengeBytes]) noexcept;
/// Parses and checks the tag under `key`.
[[nodiscard]] bool read_challenge(Span<const u8> bytes, u64 protocol, const crypto::Key& key,
                                  Challenge& out) noexcept;
/// Parses without checking: the client needs `server_ephemeral_public` to derive the key it is
/// checked under.
[[nodiscard]] bool peek_challenge(Span<const u8> bytes, u64 protocol, Challenge& out) noexcept;

// --- RESPONSE ------------------------------------------------------------------------------------

struct ResponseHeader {
    crypto::Key client_ephemeral;
    Cookie cookie;
};

[[nodiscard]] Status write_response(u64 protocol, const ResponseHeader& header,
                                    const crypto::Key& client_to_server, Span<const u8> credential,
                                    Array<u8>& out) noexcept;
[[nodiscard]] bool read_response_header(Span<const u8> bytes, u64 protocol,
                                        ResponseHeader& out) noexcept;
/// Opens the sealed credential. `out` is resized to it.
[[nodiscard]] bool open_response(Span<const u8> bytes, const crypto::Key& client_to_server,
                                 Array<u8>& out) noexcept;

// --- DATA ----------------------------------------------------------------------------------------

struct DataHeader {
    u32 connection = 0;
    u64 counter = 0;
};

[[nodiscard]] Status seal_data(const DataHeader& header, Direction direction,
                               const crypto::Key& key, Span<const u8> plain,
                               Array<u8>& out) noexcept;
[[nodiscard]] bool read_data_header(Span<const u8> bytes, DataHeader& out) noexcept;
/// Authenticates and decrypts into `plain`, resized to the ciphertext. False on a bad tag.
[[nodiscard]] bool open_data(Span<const u8> bytes, Direction direction, const crypto::Key& key,
                             Array<u8>& plain) noexcept;

/// Append one frame: kind, length, body.
[[nodiscard]] Status append_frame(FrameKind kind, Span<const u8> body, Array<u8>& out) noexcept;

}  // namespace cy::net::wire
