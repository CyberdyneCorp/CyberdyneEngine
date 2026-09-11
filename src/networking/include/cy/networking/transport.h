#pragma once
// `Transport` — the engine-defined interface for moving bytes between peers. M9 task 4.2.
//
// ================================================================================================
// WHAT IS AN INTERFACE HERE AND WHAT IS NOT
// ================================================================================================
//
// `networking-and-replication` — "Transport abstraction": connect, disconnect, send on a numbered
// channel with a delivery mode, receive, and connection state and statistics. Four delivery modes.
// "No transport library type SHALL appear outside its backend module."
//
// So this header names no socket, no address family and no library. `local_transport.h` and
// `udp_transport.h` are the two backends in this tree; a platform networking service or a QUIC
// module implements the same three virtuals and nothing above the interface changes.
//
// **The reliability layer is NOT a transport's own.** `reliability.h` implements sequencing,
// acknowledgement, retransmission, duplicate rejection and ordered delivery once, over an
// unreliable datagram substrate, and both backends compose it. A second copy inside a UDP backend
// would be a second set of window arithmetic to keep bit-compatible with the first, and the first
// thing to diverge would be the replay-protection window — the half that is a security property.
//
// ================================================================================================
// CHANNELS ARE INDEPENDENT, AND THAT IS A REQUIREMENT RATHER THAN A CONVENIENCE
// ================================================================================================
//
// "WHEN a reliable chat message is delayed by retransmission THEN unreliable state updates on
// another channel SHALL not be head-of-line blocked." A single ordered stream cannot do that, so
// each channel carries its own sequence space, its own retransmission queue and its own ordering
// buffer — see `reliability.h`. `tests/test_transport.cpp` blocks one channel and measures that the
// other still delivers, which is the requirement as a number rather than as a promise.
//
// ================================================================================================
// SECURE BY DEFAULT, STATED AS A PROPERTY THE TRANSPORT CARRIES
// ================================================================================================
//
// "Transport implementations SHALL provide, or be composed with, encryption and authentication,
// replay protection, and sequence validation; no transport SHALL be offered as a default without
// them." A transport therefore *declares* what it has, `admissible_for()` decides whether that is
// enough for the deployment asking, and a session refuses rather than shipping plaintext by
// omission. Replay protection and sequence validation are `reliability.h`'s and every backend
// composing it has them; confidentiality and peer authentication are the backend's own, and
// `UdpTransport` says plainly that it has neither yet rather than implying it does.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::net {

/// A peer, as this process addresses it. A slot plus a generation, so a disconnected peer's id does
/// not silently address whoever takes its slot next — the same argument `ecs::Entity` makes.
class PeerId {
public:
    constexpr PeerId() noexcept = default;

    [[nodiscard]] static constexpr PeerId make(u32 slot, u32 generation) noexcept {
        PeerId id;
        id.slot_ = slot;
        id.generation_ = generation;
        return id;
    }

    [[nodiscard]] constexpr u32 slot() const noexcept { return slot_; }
    [[nodiscard]] constexpr u32 generation() const noexcept { return generation_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return generation_ != 0; }
    [[nodiscard]] constexpr u64 value() const noexcept {
        return (static_cast<u64>(generation_) << 32) | slot_;
    }

    friend constexpr bool operator==(PeerId, PeerId) noexcept = default;

private:
    u32 slot_ = 0;
    u32 generation_ = 0;
};

/// The four delivery modes, in the specification's order.
enum class DeliveryMode : u8 {
    /// Send once. May be lost, duplicated or reordered. State updates, which carry their own tick.
    Unreliable = 0,
    /// Send once; a datagram older than the newest already delivered is dropped. A movement frame.
    UnreliableSequenced,
    /// Retransmitted until acknowledged; delivered in whatever order it arrives.
    ReliableUnordered,
    /// Retransmitted until acknowledged; delivered in send order.
    ReliableOrdered,
};

const char* delivery_mode_name(DeliveryMode mode) noexcept;

[[nodiscard]] constexpr bool is_reliable(DeliveryMode mode) noexcept {
    return mode == DeliveryMode::ReliableUnordered || mode == DeliveryMode::ReliableOrdered;
}

/// A numbered channel. Eight is enough for state, commands, RPCs, session control and chat with
/// room, and a small fixed count is what lets an endpoint hold its channels inline.
using ChannelId = u8;
inline constexpr u32 kMaxChannels = 8;

/// The largest payload one datagram carries, chosen to stay inside a 1200-byte safe MTU after the
/// reliability header. `networking-and-replication`: "Packets SHALL be sized to avoid IP
/// fragmentation, with a configurable MTU."
inline constexpr u32 kDefaultMtu = 1200;
inline constexpr u32 kMaxDatagramPayload = 1024;

enum class ConnectionState : u8 {
    Disconnected = 0,
    Connecting,
    /// Admitted to the transport, not yet to the session: the application's authentication step has
    /// not completed. `networking-and-replication`'s "Authentication before admission" — a peer
    /// here receives no game state.
    Authenticating,
    Connected,
    Disconnecting,
};

const char* connection_state_name(ConnectionState state) noexcept;

/// What a transport reports about one connection.
///
/// Loss is per-mille rather than a float: it is compared against a budget and printed, and a
/// diagnostic that reads `0.030000001` teaches people to distrust the diagnostic.
struct ConnectionStats {
    u32 round_trip_us = 0;
    u32 jitter_us = 0;
    u32 loss_per_mille = 0;
    u64 bytes_in = 0;
    u64 bytes_out = 0;
    u64 datagrams_in = 0;
    u64 datagrams_out = 0;
    u64 datagrams_dropped = 0;
    u64 datagrams_duplicate = 0;
    u64 retransmissions = 0;
};

/// What a transport provides against the four security obligations.
struct TransportSecurity {
    bool encrypted = false;
    bool authenticated = false;
    /// A datagram replayed by an attacker is rejected rather than re-applied.
    bool replay_protected = false;
    /// Sequence numbers are validated against a window rather than trusted.
    bool sequence_validated = false;
    /// The transport never leaves this process, so confidentiality on the wire is not a question
    /// that applies. Set by `LocalTransport` and by nothing else.
    bool in_process = false;
    /// What provides it, spelled. "" for a transport that provides nothing.
    const char* mechanism = "";
};

/// Where a transport is about to be used.
enum class Deployment : u8 {
    /// A test or a tool, in one process.
    InProcess = 0,
    /// A development build over a real network. Confidentiality is not yet required; replay
    /// protection and sequence validation are, because they are correctness as well as security.
    Development,
    /// A shipping build. Everything.
    Shipping,
};

/// Why a transport may not be used for a deployment. `None` means it may.
enum class SecurityRefusal : u8 {
    None = 0,
    NotEncrypted,
    NotAuthenticated,
    NoReplayProtection,
    NoSequenceValidation,
};

const char* security_refusal_name(SecurityRefusal refusal) noexcept;

/// "No transport SHALL be offered as a default without them", as a function.
[[nodiscard]] SecurityRefusal admissible_for(const TransportSecurity& security,
                                             Deployment deployment) noexcept;

/// One datagram, as handed to a transport or taken from one. A view: the transport copies on send
/// and owns what it hands back until the next `receive()`.
struct Datagram {
    PeerId peer;
    ChannelId channel = 0;
    DeliveryMode delivery = DeliveryMode::Unreliable;
    Span<const u8> bytes;
};

/// The interface. Three implementations in this tree and no library type in any signature.
class Transport {
public:
    Transport() noexcept = default;
    virtual ~Transport() = default;

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    /// Begin connecting to a peer named by a backend-specific string — a host and port for UDP, a
    /// registered name for the local transport. The string is the one place a backend's addressing
    /// shows through, and it is a string precisely so that no backend type appears here.
    [[nodiscard]] virtual Expected<PeerId, Error> connect(const char* address) noexcept = 0;

    virtual void disconnect(PeerId peer) noexcept = 0;

    /// Queue `bytes` for `peer` on `channel`. Refuses a payload larger than
    /// `kMaxDatagramPayload` rather than fragmenting: fragmentation belongs to the layer that knows
    /// what it is sending, and a transport that silently split a snapshot would break the
    /// "a delta SHALL never mix state from different simulation ticks" invariant invisibly.
    [[nodiscard]] virtual Status send(PeerId peer, ChannelId channel, DeliveryMode delivery,
                                      Span<const u8> bytes) noexcept = 0;

    /// Take the next datagram ready for the application, or false when there is none. The bytes are
    /// valid until the next call.
    [[nodiscard]] virtual bool receive(Datagram& out) noexcept = 0;

    /// Drive time forward: retransmit what is unacknowledged, expire what is stale, deliver what
    /// has arrived. A transport does nothing on its own — a session that stopped calling this would
    /// stop the network, visibly, rather than drifting.
    virtual void advance(u64 now_ms) noexcept = 0;

    [[nodiscard]] virtual ConnectionState state(PeerId peer) const noexcept = 0;
    [[nodiscard]] virtual ConnectionStats stats(PeerId peer) const noexcept = 0;
    [[nodiscard]] virtual TransportSecurity security() const noexcept = 0;
    /// The backend's own name, for a diagnostic. Never null.
    [[nodiscard]] virtual const char* name() const noexcept = 0;
};

// --- The network condition simulator -------------------------------------------------------------

/// `networking-and-replication` — "Network simulation": "configurable latency, jitter, packet loss,
/// duplication, and reordering, usable in development builds and tests".
///
/// **Every value is an integer and the source of randomness is seeded**, because a condition
/// simulator that used a global generator would make a failing test unreproducible — and this is
/// the milestone whose whole subject is reproducibility. `LocalNetwork` draws from
/// `determinism::RandomStream`, which is a pure function of (seed, stream, point, entity, index),
/// so the same session under the same conditions drops the same datagrams on every run.
struct NetworkConditions {
    u32 latency_ms = 0;
    /// Added uniformly in [0, jitter_ms]. What produces reordering without asking for it.
    u32 jitter_ms = 0;
    /// Per cent, 0 to 100.
    u32 loss_percent = 0;
    u32 duplication_percent = 0;
    /// Per cent of datagrams given an extra delay of `latency_ms`, so they arrive behind ones sent
    /// after them even with no jitter.
    u32 reorder_percent = 0;

    [[nodiscard]] constexpr bool adverse() const noexcept {
        return latency_ms != 0 || jitter_ms != 0 || loss_percent != 0 || duplication_percent != 0 ||
               reorder_percent != 0;
    }
};

}  // namespace cy::net
