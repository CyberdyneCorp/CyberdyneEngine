#pragma once
// The local transport: client and server in one process, over a seeded condition simulator.
// M9 tasks 4.2 and 4.5.
//
// ================================================================================================
// NOT A STAND-IN. THE SAME RELIABILITY LAYER, THE SAME FRAMING, THE SAME WINDOWS
// ================================================================================================
//
// `networking-and-replication` — "Transport swapped for tests": "WHEN an integration test runs
// client and server in one process THEN the local transport SHALL be used with configurable
// simulated latency and loss, and no gameplay code SHALL change."
//
// A local transport that delivered a `memcpy` into the peer's queue would satisfy the letter of
// that and none of its value: the behaviour a multiplayer test needs to exercise is retransmission,
// duplicate rejection, out-of-order arrival and the ordering buffer, and none of those happen in a
// `memcpy`. So `LocalNetwork` is an **unreliable datagram substrate** — it loses, duplicates,
// delays and reorders — and `LocalTransport` composes exactly the `ReliableEndpoint` that
// `UdpTransport` composes over a socket.
//
// ================================================================================================
// THE CONDITION SIMULATOR IS SEEDED, AND THAT IS THE WHOLE REASON IT IS USEFUL
// ================================================================================================
//
// A dropped datagram decided by a global generator makes a failing four-player test unreproducible,
// and this is the milestone whose subject is reproducibility. Every decision — drop, duplicate,
// delay, reorder — is a draw from `determinism::RandomStream`, which is a pure function of
// (seed, stream, point, entity, index). Two runs of the same session under the same seed drop the
// same datagrams at the same moments, so "it fails one run in five" becomes "it fails, here".
//
// The counter that indexes those draws is the **datagram ordinal**, not a wall clock and not a
// tick: two sessions that sent the same datagrams in the same order see the same network, whatever
// speed the machine ran them at.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/random.h>
#include <cy/core/memory/array.h>
#include <cy/networking/reliability.h>
#include <cy/networking/transport.h>

namespace cy::net {

/// The in-process substrate. One of these is the "network"; each `LocalTransport` is one host on
/// it.
class LocalNetwork {
public:
    LocalNetwork(Allocator& allocator, u64 seed) noexcept;
    ~LocalNetwork();

    LocalNetwork(const LocalNetwork&) = delete;
    LocalNetwork& operator=(const LocalNetwork&) = delete;

    /// Register a host under a name a `LocalTransport::connect()` can look up. The name is borrowed
    /// and must outlive the network.
    [[nodiscard]] Expected<PeerId, Error> add_host(const char* name) noexcept;

    /// The host registered under `name`, or an invalid id.
    [[nodiscard]] PeerId find_host(const char* name) const noexcept;

    void set_conditions(const NetworkConditions& conditions) noexcept { conditions_ = conditions; }
    [[nodiscard]] const NetworkConditions& conditions() const noexcept { return conditions_; }

    /// Offer one wire datagram. It may be dropped, duplicated, delayed or reordered before it
    /// arrives, and the decision is reproducible — see the header comment.
    [[nodiscard]] Status offer(PeerId from, PeerId to, u64 now_ms, Span<const u8> wire) noexcept;

    /// Move everything whose arrival time has come into its destination's inbox.
    void advance(u64 now_ms) noexcept;

    /// Take the next datagram waiting for `at`. The bytes are valid until the next call.
    [[nodiscard]] bool take(PeerId at, PeerId& from, Span<const u8>& wire) noexcept;

    [[nodiscard]] u64 offered() const noexcept { return offered_; }
    [[nodiscard]] u64 dropped() const noexcept { return dropped_; }
    [[nodiscard]] u64 duplicated() const noexcept { return duplicated_; }
    [[nodiscard]] u64 delivered() const noexcept { return delivered_; }
    [[nodiscard]] u32 in_flight() const noexcept { return static_cast<u32>(in_flight_.size()); }

private:
    struct Host;
    struct InFlight {
        u64 deliver_at_ms = 0;
        u32 from_slot = 0;
        u32 to_slot = 0;
        u32 offset = 0;
        u32 size = 0;
        /// The offering order, so two datagrams due at the same millisecond are delivered in the
        /// order they were sent. Without it the delivery order would be the order of an array whose
        /// compaction is an implementation detail.
        u64 ordinal = 0;
    };

    [[nodiscard]] Status schedule(u32 from_slot, u32 to_slot, u64 deliver_at_ms,
                                  Span<const u8> wire) noexcept;
    [[nodiscard]] u32 draw(u64 ordinal, u64 index) noexcept;

    Allocator* allocator_;
    determinism::RandomStream stream_;
    NetworkConditions conditions_{};
    Array<Host*> hosts_;
    Array<u8> arena_;
    u32 arena_base_ = 0;
    Array<InFlight> in_flight_;
    u64 next_ordinal_ = 0;
    u64 offered_ = 0;
    u64 dropped_ = 0;
    u64 duplicated_ = 0;
    u64 delivered_ = 0;
};

/// One host's view of the local network, as a `Transport`.
class LocalTransport final : public Transport {
public:
    LocalTransport(Allocator& allocator, LocalNetwork& network, PeerId self) noexcept;
    ~LocalTransport() override;

    [[nodiscard]] Expected<PeerId, Error> connect(const char* address) noexcept override;
    void disconnect(PeerId peer) noexcept override;
    [[nodiscard]] Status send(PeerId peer, ChannelId channel, DeliveryMode delivery,
                              Span<const u8> bytes) noexcept override;
    [[nodiscard]] bool receive(Datagram& out) noexcept override;
    void advance(u64 now_ms) noexcept override;
    [[nodiscard]] ConnectionState state(PeerId peer) const noexcept override;
    [[nodiscard]] ConnectionStats stats(PeerId peer) const noexcept override;
    [[nodiscard]] TransportSecurity security() const noexcept override;
    [[nodiscard]] const char* name() const noexcept override { return "local"; }

    /// Accept an inbound peer that connected to this host. The local network is symmetric — a
    /// `connect()` on one side does not create the other side's link — so a server calls this for
    /// each client it expects, which is also where its authentication policy applies.
    [[nodiscard]] Expected<PeerId, Error> accept(PeerId peer) noexcept;

    /// `networking-and-replication` — "Authentication before admission": with this set, a link
    /// stays in `Authenticating` and carries no application payload until `admit()` says so.
    void require_authentication(bool required) noexcept { authentication_required_ = required; }
    [[nodiscard]] Status admit(PeerId peer) noexcept;

    [[nodiscard]] PeerId self() const noexcept { return self_; }
    [[nodiscard]] u32 link_count() const noexcept { return static_cast<u32>(links_.size()); }
    [[nodiscard]] PeerId link_at(u32 index) const noexcept { return links_[index]->remote; }

private:
    struct Link {
        PeerId remote;
        ConnectionState state = ConnectionState::Disconnected;
        ReliableEndpoint endpoint;
        ConnectionStats stats{};

        Link(Allocator& allocator, PeerId peer, ConnectionState initial) noexcept
            : remote(peer), state(initial), endpoint(allocator) {}
    };

    [[nodiscard]] Link* find(PeerId peer) noexcept;
    [[nodiscard]] const Link* find(PeerId peer) const noexcept;
    [[nodiscard]] Expected<Link*, Error> open(PeerId peer, ConnectionState initial) noexcept;
    [[nodiscard]] bool drain(Datagram& out) noexcept;
    void flush(u64 now_ms) noexcept;

    Allocator* allocator_;
    LocalNetwork* network_;
    PeerId self_;
    Array<Link*> links_;
    Array<u8> outgoing_;
    u32 drain_link_ = 0;
    u32 drain_channel_ = 0;
    u64 now_ms_ = 0;
    bool authentication_required_ = false;
};

}  // namespace cy::net
