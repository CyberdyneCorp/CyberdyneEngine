#pragma once
// The reliability layer: one implementation, composed by every backend. M9 task 4.2.
//
// ================================================================================================
// WHY THIS IS NOT INSIDE THE UDP BACKEND
// ================================================================================================
//
// `networking-and-replication` asks for "a UDP transport with its own reliability layer" and, four
// requirements later, for "replay protection, and sequence validation" from every transport.
// Written inside the UDP backend, the second of those would have to be written again for the local
// transport and again for the next one — and the first thing to diverge between two copies of
// window arithmetic is the replay-protection window, which is the half that is a security property
// rather than a convenience.
//
// So it is here, over an unreliable datagram substrate, and `LocalTransport` and `UdpTransport`
// both compose it. The local transport is then not a simplified stand-in for the real one: it runs
// the same sequencing, the same acknowledgement, the same retransmission and the same duplicate
// rejection, which is what makes "WHEN an integration test runs client and server in one process
// THEN ... no gameplay code SHALL change" true of the reliability behaviour too.
//
// ================================================================================================
// ONE SEQUENCE SPACE PER CHANNEL
// ================================================================================================
//
// "WHEN a reliable chat message is delayed by retransmission THEN unreliable state updates on
// another channel SHALL not be head-of-line blocked." A channel therefore owns its own sequence
// counter, its own unacknowledged queue and its own ordering buffer. Nothing is shared between
// channels except the peer.
//
// Sequence numbers are 16-bit and wrap. `sequence_newer()` compares them the only way that is
// correct across a wrap — by the sign of the difference in the modular space — because a
// `>` between two wrapping counters is a bug that appears once every 65 536 datagrams and is
// impossible to reproduce from a report.
//
// ================================================================================================
// THE ORDERED CHANNEL DOES NOT USE THE REPLAY WINDOW, AND THAT IS A FIX RATHER THAN A SHORTCUT
// ================================================================================================
//
// M9 declared `reliable-channel-stalls-under-loss`: "a reliable-ordered channel stops delivering to
// the application under sustained heavy loss and nothing detects it: the frontier freezes, the
// transport keeps delivering, `abandoned()` has no caller". The cause was here, and it was not a
// missing detector — it was the **replay window being narrower than the retransmission horizon**.
//
// `already_received()` treats anything more than `kReplayWindow` (32) sequences behind the newest
// arrival as seen, because a receiver that can no longer prove it has not already applied a
// datagram must refuse it. That is right for an unreliable datagram, which is never sent twice. It
// is wrong for a reliable one, which is sent again and again: at 60 Hz with one reliable datagram a
// tick, the default `RetransmitPolicy` spreads ten attempts over 7.5 s — about 470 sequences — so
// the third attempt onwards lands OUTSIDE the window and is called a replay. `next_ordered_` then
// never advances again, every later datagram piles into the ordering buffer, and the transport goes
// on delivering while the application hears nothing.
//
// An ordered channel does not need a window, because it has an EXACT record of what it has taken:
// everything strictly older than `next_ordered_` was delivered, and everything from `next_ordered_`
// up is either held in `reorder_` or has never been seen. `already_taken_ordered()` asks that
// record, so a retransmission is accepted however late it is, and a genuine replay is still
// refused — exactly, rather than conservatively.
//
// The other half is the acknowledgement. `ack`/`ack_bits` describe 32 sequences, so a receiver that
// has just accepted one 470 behind the newest cannot say so, and the sender would retransmit until
// it abandoned the peer. So such a sequence is remembered and acknowledged by NAME, in a bare
// datagram whose `ack` field is that sequence — which `acknowledge()` already understands, so
// nothing on the wire changes.
//
// **`ReliableUnordered` still uses the window**, and a retransmission of one that arrives more than
// 32 sequences late is still refused. It has no frontier to be exact against, and no caller in this
// tree; the signal that it has gone wrong is `abandoned()`, which now reaches the application
// through `ConnectionStats::reliable_abandoned`.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/networking/transport.h>

namespace cy::net {

/// The wire header every datagram carries. Twelve bytes, little-endian, field by field —
/// **never a memcpy of the struct**, for the reason `replay::encode()` gives: a struct's padding is
/// whatever the last thing to occupy those bytes left behind, and two peers that agree about every
/// value would then disagree about the bytes.
struct DatagramHeader {
    u16 sequence = 0;
    /// The newest sequence this side has received on this channel.
    u16 ack = 0;
    /// The 32 sequences before `ack`, one bit each, newest first. One acknowledgement therefore
    /// covers a burst of loss without a round trip per datagram.
    u32 ack_bits = 0;
    ChannelId channel = 0;
    DeliveryMode delivery = DeliveryMode::Unreliable;
    u16 payload_size = 0;
};

inline constexpr u32 kDatagramHeaderSize = 12;
inline constexpr u32 kMaxWireDatagram = kDatagramHeaderSize + kMaxDatagramPayload;

void encode_header(const DatagramHeader& header, u8* out) noexcept;
[[nodiscard]] bool decode_header(Span<const u8> bytes, DatagramHeader& out) noexcept;

/// Is `candidate` newer than `reference` in a wrapping 16-bit sequence space?
[[nodiscard]] constexpr bool sequence_newer(u16 candidate, u16 reference) noexcept {
    return static_cast<i16>(static_cast<u16>(candidate - reference)) > 0;
}

/// What the receive side decided about one arriving datagram.
enum class ReceiveVerdict : u8 {
    /// Handed to the application, now or once ordering allowed it.
    Delivered = 0,
    /// Already seen, and what an attacker's replayed datagram gets. The replay window's answer for
    /// an unreliable datagram; the delivery frontier's exact one for `ReliableOrdered`.
    Duplicate,
    /// Superseded under `UnreliableSequenced`. Never `ReliableOrdered`'s answer since M10: an
    /// ordered sequence older than the frontier has been delivered, which is `Duplicate`.
    TooOld,
    /// Held until the gap before it is filled. `ReliableOrdered` only.
    Buffered,
    /// A bare acknowledgement, carrying no payload.
    AckOnly,
};

const char* receive_verdict_name(ReceiveVerdict verdict) noexcept;

/// How long before an unacknowledged datagram is sent again, and how many times.
struct RetransmitPolicy {
    u32 initial_timeout_ms = 100;
    /// Doubled per attempt up to this.
    u32 maximum_timeout_ms = 1000;
    /// After this many attempts the datagram is abandoned and the peer is reported unreachable.
    /// Bounded rather than infinite: an unbounded retransmit queue is how a dead peer consumes a
    /// server's memory.
    u32 maximum_attempts = 10;
};

/// One channel of one connection.
///
/// Owns two byte arenas — one for what is waiting to be acknowledged, one for what is waiting to be
/// delivered — and compacts each from the front when its head advances. An arena rather than an
/// `Array<u8>` per datagram, because a retransmission queue of a hundred datagrams would otherwise
/// be a hundred allocations at exactly the moment the network is already in trouble.
class ReliabilityChannel {
public:
    ReliabilityChannel(Allocator& allocator, ChannelId channel) noexcept;

    ReliabilityChannel(const ReliabilityChannel&) = delete;
    ReliabilityChannel& operator=(const ReliabilityChannel&) = delete;

    /// Frame `payload` for sending: writes header and payload into `out`, and remembers the
    /// datagram for retransmission when `delivery` is reliable.
    [[nodiscard]] Status frame(DeliveryMode delivery, Span<const u8> payload, u64 now_ms,
                               Array<u8>& out) noexcept;

    /// A bare acknowledgement, for when this side has received something and has nothing to send.
    [[nodiscard]] Status frame_ack(Array<u8>& out) noexcept;

    /// One bare acknowledgement per sequence this side has taken but cannot describe in
    /// `ack`/`ack_bits` — see the header comment. Each names its sequence in the `ack` field and
    /// carries no payload. Drains the list, so a lost one is re-recorded by the peer's next
    /// retransmission rather than remembered forever.
    [[nodiscard]] Status frame_named_acks(Array<u8>& out) noexcept;

    /// How many sequences are waiting to be acknowledged by name. Zero on a channel whose peer is
    /// keeping up; non-zero is the measurement that the retransmission horizon has outrun the
    /// acknowledgement window.
    [[nodiscard]] u32 named_acks_pending() const noexcept {
        return static_cast<u32>(named_acks_.size());
    }

    /// Take one arriving datagram, already parsed. Applies the peer's acknowledgement, rejects a
    /// duplicate or a replay, and queues the payload for delivery when it is due.
    [[nodiscard]] Status accept(const DatagramHeader& header, Span<const u8> payload,
                                ReceiveVerdict& verdict) noexcept;

    /// The next payload ready for the application, oldest first. The bytes are valid until the next
    /// call to anything on this channel.
    [[nodiscard]] bool next_delivery(Span<const u8>& out, DeliveryMode& delivery) noexcept;

    /// Datagrams whose retransmission is due, appended to `out` as complete wire datagrams
    /// back to back, each `kDatagramHeaderSize + payload_size` bytes.
    [[nodiscard]] Status collect_retransmissions(u64 now_ms, const RetransmitPolicy& policy,
                                                 Array<u8>& out) noexcept;

    /// True once a datagram has exhausted `maximum_attempts`. The connection is then over, and
    /// saying so is better than retrying forever.
    [[nodiscard]] bool abandoned() const noexcept { return abandoned_; }

    [[nodiscard]] u32 unacknowledged() const noexcept { return static_cast<u32>(unacked_.size()); }
    [[nodiscard]] u32 buffered() const noexcept { return static_cast<u32>(reorder_.size()); }
    [[nodiscard]] u64 retransmissions() const noexcept { return retransmissions_; }
    [[nodiscard]] u64 duplicates_rejected() const noexcept { return duplicates_; }
    [[nodiscard]] u64 delivered() const noexcept { return delivered_; }
    /// True when this side owes the peer an acknowledgement it has not yet piggybacked.
    [[nodiscard]] bool owes_ack() const noexcept { return owes_ack_; }

    void clear() noexcept;

private:
    struct Pending {
        u16 sequence = 0;
        u32 offset = 0;
        u32 size = 0;
        u32 attempts = 1;
        u64 next_attempt_ms = 0;
        DeliveryMode delivery = DeliveryMode::ReliableOrdered;
    };
    struct Queued {
        u16 sequence = 0;
        u32 offset = 0;
        u32 size = 0;
        /// Carried through so the transport can report what a payload arrived as. A caller that had
        /// to remember the mode it sent under would be a caller keeping a second copy of the state
        /// the header already carries.
        DeliveryMode delivery = DeliveryMode::Unreliable;
    };

    [[nodiscard]] DatagramHeader outgoing_header(DeliveryMode delivery, u16 sequence,
                                                 u16 payload_size) noexcept;
    [[nodiscard]] Status remember(u16 sequence, DeliveryMode delivery, Span<const u8> payload,
                                  u64 now_ms) noexcept;
    void acknowledge(u16 ack, u32 ack_bits) noexcept;
    void drop_acknowledged(u16 sequence) noexcept;
    void compact_send_arena() noexcept;
    void compact_delivery_arena() noexcept;
    [[nodiscard]] bool already_received(u16 sequence) const noexcept;
    /// The ordered channel's exact answer to "have I taken this already?", which needs no window.
    [[nodiscard]] bool already_taken_ordered(u16 sequence) const noexcept;
    /// Is `sequence` too far behind the newest arrival for `ack_bits` to describe it?
    [[nodiscard]] bool beyond_ack_window(u16 sequence) const noexcept;
    [[nodiscard]] Status note_named_ack(u16 sequence) noexcept;
    void note_received(u16 sequence) noexcept;
    [[nodiscard]] Status accept_ordered(const DatagramHeader& header, Span<const u8> payload,
                                        ReceiveVerdict& verdict) noexcept;
    [[nodiscard]] Status queue_for_delivery(u16 sequence, DeliveryMode delivery,
                                            Span<const u8> payload) noexcept;
    [[nodiscard]] Status buffer_for_ordering(u16 sequence, DeliveryMode delivery,
                                             Span<const u8> payload) noexcept;
    [[nodiscard]] Status drain_ordered() noexcept;

    ChannelId channel_;
    u16 next_sequence_ = 1;
    /// The newest sequence received, and the 32 before it. The replay window.
    u16 newest_received_ = 0;
    u32 received_bits_ = 0;
    bool any_received_ = false;
    /// The next sequence `ReliableOrdered` will hand to the application.
    u16 next_ordered_ = 1;
    bool owes_ack_ = false;
    bool abandoned_ = false;

    Array<u8> send_arena_;
    u32 send_base_ = 0;
    Array<Pending> unacked_;

    Array<u8> delivery_arena_;
    u32 delivery_base_ = 0;
    Array<Queued> delivery_;
    u32 delivery_head_ = 0;

    Array<u8> reorder_arena_;
    Array<Queued> reorder_;

    /// Sequences to acknowledge by name on the next collection. Bounded: the list is drained every
    /// time the transport advances, and an unbounded one would be a receiver's memory spent on a
    /// sender that is already in trouble.
    Array<u16> named_acks_;

    u64 retransmissions_ = 0;
    u64 duplicates_ = 0;
    u64 delivered_ = 0;
};

/// One connection's eight channels, plus the framing every backend needs.
///
/// A backend owns one of these per peer and does three things with it: `frame()` on send,
/// `ingest()` on receive, and `collect_retransmissions()` on `advance()`. Everything else — the
/// socket, the address, the condition simulator — is the backend's.
class ReliableEndpoint {
public:
    explicit ReliableEndpoint(Allocator& allocator) noexcept;

    ReliableEndpoint(const ReliableEndpoint&) = delete;
    ReliableEndpoint& operator=(const ReliableEndpoint&) = delete;

    [[nodiscard]] Status frame(ChannelId channel, DeliveryMode delivery, Span<const u8> payload,
                               u64 now_ms, Array<u8>& out) noexcept;

    /// Parse one wire datagram and route it. Refuses a datagram whose header does not decode or
    /// whose channel is out of range — **sequence validation**, and the reason a malformed datagram
    /// is a rejection with a name rather than a read past the end of a buffer.
    [[nodiscard]] Status ingest(Span<const u8> wire, ChannelId& channel,
                                ReceiveVerdict& verdict) noexcept;

    [[nodiscard]] bool next_delivery(ChannelId channel, Span<const u8>& out,
                                     DeliveryMode& delivery) noexcept;

    /// Every channel's due retransmissions and owed acknowledgements, appended to `out` back to
    /// back. The caller sends them; this class never touches a socket.
    [[nodiscard]] Status collect_outgoing(u64 now_ms, Array<u8>& out) noexcept;

    [[nodiscard]] ReliabilityChannel& channel(ChannelId index) noexcept {
        return *channels_[index];
    }
    [[nodiscard]] const ReliabilityChannel& channel(ChannelId index) const noexcept {
        return *channels_[index];
    }

    [[nodiscard]] bool abandoned() const noexcept;
    [[nodiscard]] u64 retransmissions() const noexcept;
    [[nodiscard]] u64 duplicates_rejected() const noexcept;

    void set_policy(const RetransmitPolicy& policy) noexcept { policy_ = policy; }
    [[nodiscard]] const RetransmitPolicy& policy() const noexcept { return policy_; }

    [[nodiscard]] Status prepare() noexcept;

    ~ReliableEndpoint();

private:
    Allocator* allocator_;
    ReliabilityChannel* channels_[kMaxChannels] = {};
    RetransmitPolicy policy_{};
};

}  // namespace cy::net
