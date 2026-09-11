#pragma once
// The UDP transport: a real socket under the same reliability layer. M9 task 4.2.
//
// ================================================================================================
// WHAT IS HERE, AND WHAT IS DELIBERATELY NOT
// ================================================================================================
//
// `networking-and-replication` asks the engine to ship "a **UDP transport** with its own
// reliability layer, a **WebSocket transport** for browser targets, and a **local transport** for
// single-process testing". Two of the three are here and in `local_transport.h`. **The WebSocket
// transport is not implemented**, and that is recorded as a gap rather than implied by its absence:
// this tree has no browser target and nothing that could exercise one, so writing it would be
// writing code no test can run — which is how the previous milestones acquired the defects their
// gates found. `src/networking/README.md` names it as deferred, with what it would cost.
//
// **The reliability layer is not this file's.** It is `reliability.h`'s, composed here exactly as
// `LocalTransport` composes it, which is what makes the in-process test a real test of the
// behaviour and not a test of a simplification.
//
// ================================================================================================
// WHAT THIS TRANSPORT DOES NOT PROVIDE, SAID PLAINLY
// ================================================================================================
//
// `security()` reports **no encryption and no peer authentication**. It reports replay protection
// and sequence validation, because `reliability.h` provides both. `admissible_for()` therefore
// refuses this transport for a `Shipping` deployment, by returning `NotEncrypted` — which is the
// specification's "no transport SHALL be offered as a default without them" as a refusal rather
// than as an intention. DTLS is the named answer and it is a dependency this milestone did not
// take; `README.md` records that too.
//
// ================================================================================================
// ONE ARCHITECTURE, ONE OPERATING SYSTEM
// ================================================================================================
//
// The socket half is POSIX and is compiled on Linux and macOS. On every other platform `open()`
// returns `Unsupported` rather than failing to link, and **Windows is reported unverified** — this
// machine has one operating system and nothing here has been run on another.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/networking/reliability.h>
#include <cy/networking/transport.h>

namespace cy::net {

/// True when this build has a socket implementation. False on Windows, where `open()` refuses
/// rather than pretending.
[[nodiscard]] bool udp_available() noexcept;

/// An address, parsed. IPv4 only: an IPv6 path would be a second code path with no test on this
/// host, and the honest position is one that is exercised.
struct UdpAddress {
    u32 ipv4 = 0;
    u16 port = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return port != 0; }
};

/// Parse "a.b.c.d:port". Returns an invalid address on anything else, rather than a partial one.
[[nodiscard]] UdpAddress parse_udp_address(const char* text) noexcept;

class UdpTransport final : public Transport {
public:
    explicit UdpTransport(Allocator& allocator) noexcept;
    ~UdpTransport() override;

    /// Bind. Port zero takes an ephemeral one, which `bound_port()` then reports — the only way a
    /// test can run two of these without agreeing a port number in advance.
    [[nodiscard]] Status open(u16 port) noexcept;
    void close() noexcept;
    [[nodiscard]] u16 bound_port() const noexcept { return bound_port_; }
    [[nodiscard]] bool is_open() const noexcept { return socket_ >= 0; }

    [[nodiscard]] Expected<PeerId, Error> connect(const char* address) noexcept override;
    void disconnect(PeerId peer) noexcept override;
    [[nodiscard]] Status send(PeerId peer, ChannelId channel, DeliveryMode delivery,
                              Span<const u8> bytes) noexcept override;
    [[nodiscard]] bool receive(Datagram& out) noexcept override;
    void advance(u64 now_ms) noexcept override;
    [[nodiscard]] ConnectionState state(PeerId peer) const noexcept override;
    [[nodiscard]] ConnectionStats stats(PeerId peer) const noexcept override;
    [[nodiscard]] TransportSecurity security() const noexcept override;
    [[nodiscard]] const char* name() const noexcept override { return "udp"; }

    /// Accept an inbound peer at a known address, the way a server does for a client whose
    /// handshake it has already seen.
    [[nodiscard]] Expected<PeerId, Error> accept(const UdpAddress& address) noexcept;

    [[nodiscard]] u32 link_count() const noexcept { return static_cast<u32>(links_.size()); }
    [[nodiscard]] u64 datagrams_sent() const noexcept { return sent_; }
    [[nodiscard]] u64 datagrams_received() const noexcept { return received_; }
    /// Datagrams from an address no link names. Counted rather than processed: an unsolicited
    /// datagram is the cheapest denial-of-service there is, and answering one is how it works.
    [[nodiscard]] u64 datagrams_from_strangers() const noexcept { return strangers_; }

private:
    struct Link {
        PeerId remote;
        UdpAddress address;
        ConnectionState state = ConnectionState::Disconnected;
        ReliableEndpoint endpoint;
        ConnectionStats stats{};

        Link(Allocator& allocator, PeerId peer, const UdpAddress& at) noexcept
            : remote(peer), address(at), state(ConnectionState::Connected), endpoint(allocator) {}
    };

    [[nodiscard]] Link* find(PeerId peer) noexcept;
    [[nodiscard]] const Link* find(PeerId peer) const noexcept;
    [[nodiscard]] Link* find(const UdpAddress& address) noexcept;
    [[nodiscard]] Expected<Link*, Error> open_link(const UdpAddress& address) noexcept;
    [[nodiscard]] bool drain(Datagram& out) noexcept;
    [[nodiscard]] bool pump() noexcept;
    void flush(u64 now_ms) noexcept;
    [[nodiscard]] Status transmit(const UdpAddress& address, Span<const u8> wire) noexcept;

    Allocator* allocator_;
    Array<Link*> links_;
    Array<u8> outgoing_;
    Array<u8> inbound_;
    int socket_ = -1;
    u16 bound_port_ = 0;
    u32 next_slot_ = 0;
    u32 drain_link_ = 0;
    u32 drain_channel_ = 0;
    u64 now_ms_ = 0;
    u64 sent_ = 0;
    u64 received_ = 0;
    u64 strangers_ = 0;
};

}  // namespace cy::net
