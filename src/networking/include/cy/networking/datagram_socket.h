// SPDX-License-Identifier: MIT
#pragma once
// An unreliable datagram substrate: a real UDP socket, or the seeded simulated network. Issue #82.
//
// ================================================================================================
// WHY THE SECURE TRANSPORT SITS ON THIS AND NOT ON A SOCKET
// ================================================================================================
//
// `networking-and-replication` — "Transport swapped for tests": an integration test runs client and
// server in one process "with configurable simulated latency and loss, and no gameplay code SHALL
// change". `SecureTransport` therefore never sees a socket. It sees a `DatagramSocket`, which is
// either `UdpSocket` (POSIX or Winsock, IPv4) or `SimulatedSocket` (one host on a `LocalNetwork`,
// whose latency, jitter, loss, duplication and reordering are seeded draws). The handshake, the
// encryption, the replay window, fragmentation and congestion control all run unchanged over
// both, so a test of them over the simulated network is a test of what ships.
//
// An address is two integers rather than a `sockaddr`: no socket type crosses this header, which
// is the specification's "no transport library type SHALL appear outside its backend module".

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/networking/local_transport.h>

namespace cy::net {

/// Where a datagram goes or came from. For UDP, `host` is an IPv4 address in host byte order; for
/// the simulated network it is the host's `PeerId::value()` and `port` is 1.
struct SocketAddress {
    u64 host = 0;
    u16 port = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return port != 0; }
    friend constexpr bool operator==(const SocketAddress&, const SocketAddress&) noexcept = default;
    /// One integer, for a hash map key.
    [[nodiscard]] constexpr u64 key() const noexcept { return (host << 16) ^ port; }
};

/// The substrate. Datagrams may be lost, duplicated, delayed and reordered; nothing above relies on
/// the opposite.
class DatagramSocket {
public:
    DatagramSocket() noexcept = default;
    virtual ~DatagramSocket() = default;

    DatagramSocket(const DatagramSocket&) = delete;
    DatagramSocket& operator=(const DatagramSocket&) = delete;

    /// Turn a backend-specific name into an address: "a.b.c.d:port" for UDP, a registered host
    /// name for the simulated network.
    [[nodiscard]] virtual Expected<SocketAddress, Error> resolve(
        const char* text) const noexcept = 0;

    [[nodiscard]] virtual Status send_to(const SocketAddress& to,
                                         Span<const u8> bytes) noexcept = 0;

    /// The next datagram that has arrived, or false. The bytes are valid until the next call.
    [[nodiscard]] virtual bool receive_from(SocketAddress& from,
                                            Span<const u8>& bytes) noexcept = 0;

    /// Drive time. A real socket ignores it; the simulated network delivers what is due.
    virtual void advance(u64 now_ms) noexcept { (void)now_ms; }

    /// True when datagrams never leave this process.
    [[nodiscard]] virtual bool in_process() const noexcept = 0;

    [[nodiscard]] virtual const char* name() const noexcept = 0;
};

/// The largest datagram either backend carries. Above the 1200-byte safe MTU on purpose: a
/// receiver must be able to read, count and refuse an oversized datagram rather than truncate it.
inline constexpr u32 kMaxSocketDatagram = 2048;

/// A UDP socket over IPv4. POSIX on Linux and macOS, Winsock on Windows.
class UdpSocket final : public DatagramSocket {
public:
    explicit UdpSocket(Allocator& allocator) noexcept;
    ~UdpSocket() override;

    /// Bind. Port zero takes an ephemeral one, which `bound_port()` reports.
    [[nodiscard]] Status open(u16 port) noexcept;
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept { return handle_ != kClosed; }
    [[nodiscard]] u16 bound_port() const noexcept { return bound_port_; }

    [[nodiscard]] Expected<SocketAddress, Error> resolve(const char* text) const noexcept override;
    [[nodiscard]] Status send_to(const SocketAddress& to, Span<const u8> bytes) noexcept override;
    [[nodiscard]] bool receive_from(SocketAddress& from, Span<const u8>& bytes) noexcept override;
    [[nodiscard]] bool in_process() const noexcept override { return false; }
    [[nodiscard]] const char* name() const noexcept override { return "udp"; }

private:
    /// Wide enough for a 64-bit Winsock `SOCKET` and a POSIX descriptor alike.
    static constexpr i64 kClosed = -1;

    Array<u8> inbound_;
    i64 handle_ = kClosed;
    u16 bound_port_ = 0;
};

/// One host on a `LocalNetwork`. The network is shared; each socket registers its own name.
class SimulatedSocket final : public DatagramSocket {
public:
    SimulatedSocket(LocalNetwork& network, PeerId self) noexcept
        : network_(&network), self_(self) {}

    /// Register `name` on `network` and return its socket. The name is borrowed.
    [[nodiscard]] static Expected<PeerId, Error> register_host(LocalNetwork& network,
                                                               const char* name) noexcept {
        return network.add_host(name);
    }

    [[nodiscard]] Expected<SocketAddress, Error> resolve(const char* text) const noexcept override;
    [[nodiscard]] Status send_to(const SocketAddress& to, Span<const u8> bytes) noexcept override;
    [[nodiscard]] bool receive_from(SocketAddress& from, Span<const u8>& bytes) noexcept override;
    void advance(u64 now_ms) noexcept override;
    [[nodiscard]] bool in_process() const noexcept override { return true; }
    [[nodiscard]] const char* name() const noexcept override { return "simulated"; }

    [[nodiscard]] SocketAddress address() const noexcept { return {self_.value(), 1}; }
    [[nodiscard]] LocalNetwork& network() const noexcept { return *network_; }

private:
    LocalNetwork* network_;
    PeerId self_;
    u64 now_ms_ = 0;
};

}  // namespace cy::net
