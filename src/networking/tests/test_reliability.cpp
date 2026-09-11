// M9 TASK 4.2 — SEQUENCING, ACKNOWLEDGEMENT, RETRANSMISSION, AND THE REPLAY WINDOW.
//
// The cases here drive `ReliabilityChannel` directly rather than through a transport, because the
// interesting states — a duplicate, a datagram older than the window, a gap that the ordering
// buffer holds — are states a transport reaches only by chance.
//
// `networking-and-replication`'s "Channel independence" scenario is the last case: a reliable
// channel blocked by retransmission must not stop an unreliable one on another channel. That is the
// requirement as a count of delivered datagrams, not as an architectural claim.

#include "fixture.h"

#include <cy/networking/reliability.h>

using namespace cy::net_test;
using cy::u32;
using cy::u64;
using cy::u8;

namespace {

/// Move every datagram in `wire` from one endpoint to the other, optionally dropping the nth.
/// Returns how many were delivered.
[[nodiscard]] u32 deliver(cy::Array<u8>& wire, ReliableEndpoint& into, u32 drop_index) noexcept {
    u32 delivered = 0;
    u32 index = 0;
    cy::usize cursor = 0;
    while (cursor + kDatagramHeaderSize <= wire.size()) {
        DatagramHeader header;
        const cy::Span<const u8> rest(wire.data() + cursor, wire.size() - cursor);
        if (!decode_header(rest, header)) {
            break;
        }
        const cy::usize length = kDatagramHeaderSize + header.payload_size;
        if (index != drop_index) {
            ChannelId channel = 0;
            ReceiveVerdict verdict = ReceiveVerdict::Delivered;
            if (into.ingest(cy::Span<const u8>(wire.data() + cursor, length), channel, verdict)) {
                ++delivered;
            }
        }
        cursor += length;
        ++index;
    }
    return delivered;
}

[[nodiscard]] u32 drain(ReliableEndpoint& endpoint, ChannelId channel) noexcept {
    u32 count = 0;
    cy::Span<const u8> payload;
    DeliveryMode delivery = DeliveryMode::Unreliable;
    while (endpoint.next_delivery(channel, payload, delivery)) {
        ++count;
    }
    return count;
}

constexpr u8 kPayload[4] = {1, 2, 3, 4};

}  // namespace

CY_TEST_CASE("networking: the header round-trips field by field, and refuses what it cannot read") {
    DatagramHeader header;
    header.sequence = 0xBEEF;
    header.ack = 0x1234;
    header.ack_bits = 0xDEAD'C0DEU;
    header.channel = 5;
    header.delivery = DeliveryMode::ReliableOrdered;
    header.payload_size = 4;

    u8 bytes[kDatagramHeaderSize + 4] = {};
    encode_header(header, bytes);
    DatagramHeader read;
    CY_REQUIRE(decode_header(cy::Span<const u8>(bytes, sizeof(bytes)), read));
    CY_CHECK_EQ(read.sequence, header.sequence);
    CY_CHECK_EQ(read.ack, header.ack);
    CY_CHECK_EQ(read.ack_bits, header.ack_bits);
    CY_CHECK_EQ(read.channel, header.channel);
    CY_CHECK(read.delivery == header.delivery);
    CY_CHECK_EQ(read.payload_size, header.payload_size);

    // A buffer that claims a payload it does not carry is refused rather than read past. That is
    // sequence validation's least glamorous half and the one an attacker reaches first.
    CY_CHECK_FALSE(decode_header(cy::Span<const u8>(bytes, kDatagramHeaderSize + 2), read));
    CY_CHECK_FALSE(decode_header(cy::Span<const u8>(bytes, 4), read));
    bytes[9] = 99;  // not a delivery mode
    CY_CHECK_FALSE(decode_header(cy::Span<const u8>(bytes, sizeof(bytes)), read));
}

CY_TEST_CASE("networking: sequence comparison is correct across the wrap") {
    CY_CHECK(sequence_newer(2, 1));
    CY_CHECK_FALSE(sequence_newer(1, 2));
    // The case a `>` gets wrong once every 65 536 datagrams and that nobody can reproduce from a
    // report.
    CY_CHECK(sequence_newer(1, 65535));
    CY_CHECK_FALSE(sequence_newer(65535, 1));
    CY_CHECK_FALSE(sequence_newer(7, 7));
}

CY_TEST_CASE("networking: a replayed datagram is rejected by the window") {
    ReliableEndpoint sender(allocator());
    ReliableEndpoint receiver(allocator());
    CY_REQUIRE(sender.prepare().has_value());
    CY_REQUIRE(receiver.prepare().has_value());

    cy::Array<u8> wire(allocator());
    CY_REQUIRE(sender.frame(0, DeliveryMode::Unreliable, cy::Span<const u8>(kPayload, 4), 0, wire)
                   .has_value());

    ChannelId channel = 0;
    ReceiveVerdict verdict = ReceiveVerdict::Duplicate;
    CY_REQUIRE(receiver.ingest(wire.span(), channel, verdict).has_value());
    CY_CHECK(verdict == ReceiveVerdict::Delivered);

    // The same bytes again — an attacker's replay, or a duplicating router.
    CY_REQUIRE(receiver.ingest(wire.span(), channel, verdict).has_value());
    CY_CHECK(verdict == ReceiveVerdict::Duplicate);
    CY_CHECK_EQ(receiver.duplicates_rejected(), u64{1});
    CY_CHECK_EQ(drain(receiver, 0), 1U);
}

CY_TEST_CASE("networking: an ordered channel holds a gap and releases it in order") {
    ReliableEndpoint sender(allocator());
    ReliableEndpoint receiver(allocator());
    CY_REQUIRE(sender.prepare().has_value());
    CY_REQUIRE(receiver.prepare().has_value());

    cy::Array<u8> wire(allocator());
    for (u32 index = 0; index < 4; ++index) {
        const u8 payload[1] = {static_cast<u8>(index)};
        CY_REQUIRE(
            sender.frame(1, DeliveryMode::ReliableOrdered, cy::Span<const u8>(payload, 1), 0, wire)
                .has_value());
    }

    // Drop the second. The first is delivered; the third and fourth wait.
    CY_CHECK_EQ(deliver(wire, receiver, /*drop_index=*/1), 3U);
    CY_CHECK_EQ(drain(receiver, 1), 1U);
    CY_CHECK_EQ(receiver.channel(1).buffered(), 2U);

    // Retransmission fills the gap and the buffer empties in order.
    cy::Array<u8> again(allocator());
    CY_REQUIRE(sender.collect_outgoing(1000, again).has_value());
    CY_CHECK_GT(sender.retransmissions(), u64{0});
    CY_CHECK_GT(deliver(again, receiver, /*drop_index=*/0xFFFF'FFFFU), 0U);
    CY_CHECK_EQ(drain(receiver, 1), 3U);
    CY_CHECK_EQ(receiver.channel(1).buffered(), 0U);
}

CY_TEST_CASE("networking: an unreliable-sequenced datagram older than the newest is dropped") {
    ReliableEndpoint sender(allocator());
    ReliableEndpoint receiver(allocator());
    CY_REQUIRE(sender.prepare().has_value());
    CY_REQUIRE(receiver.prepare().has_value());

    cy::Array<u8> first(allocator());
    cy::Array<u8> second(allocator());
    const u8 a[1] = {1};
    const u8 b[1] = {2};
    CY_REQUIRE(
        sender.frame(2, DeliveryMode::UnreliableSequenced, cy::Span<const u8>(a, 1), 0, first)
            .has_value());
    CY_REQUIRE(
        sender.frame(2, DeliveryMode::UnreliableSequenced, cy::Span<const u8>(b, 1), 0, second)
            .has_value());

    ChannelId channel = 0;
    ReceiveVerdict verdict = ReceiveVerdict::Delivered;
    // Arriving out of order, which is what an unreliable channel does.
    CY_REQUIRE(receiver.ingest(second.span(), channel, verdict).has_value());
    CY_CHECK(verdict == ReceiveVerdict::Delivered);
    CY_REQUIRE(receiver.ingest(first.span(), channel, verdict).has_value());
    CY_CHECK(verdict == ReceiveVerdict::TooOld);
    CY_CHECK_EQ(drain(receiver, 2), 1U);
}

CY_TEST_CASE("networking: an acknowledgement clears the retransmission queue") {
    ReliableEndpoint alice(allocator());
    ReliableEndpoint bob(allocator());
    CY_REQUIRE(alice.prepare().has_value());
    CY_REQUIRE(bob.prepare().has_value());

    cy::Array<u8> wire(allocator());
    for (u32 index = 0; index < 3; ++index) {
        CY_REQUIRE(
            alice
                .frame(0, DeliveryMode::ReliableUnordered, cy::Span<const u8>(kPayload, 4), 0, wire)
                .has_value());
    }
    CY_CHECK_EQ(alice.channel(0).unacknowledged(), 3U);
    CY_CHECK_EQ(deliver(wire, bob, 0xFFFF'FFFFU), 3U);

    // Bob owes an acknowledgement and sends one on the next advance; Alice's queue empties.
    CY_CHECK(bob.channel(0).owes_ack());
    cy::Array<u8> acks(allocator());
    CY_REQUIRE(bob.collect_outgoing(10, acks).has_value());
    CY_CHECK_GT(acks.size(), cy::usize{0});
    CY_CHECK_EQ(deliver(acks, alice, 0xFFFF'FFFFU), 1U);
    CY_CHECK_EQ(alice.channel(0).unacknowledged(), 0U);

    // And nothing is retransmitted afterwards, however long the clock runs.
    cy::Array<u8> nothing(allocator());
    CY_REQUIRE(alice.collect_outgoing(100000, nothing).has_value());
    CY_CHECK_EQ(alice.retransmissions(), u64{0});
}

CY_TEST_CASE("networking: a blocked reliable channel does not block another channel") {
    ReliableEndpoint sender(allocator());
    ReliableEndpoint receiver(allocator());
    CY_REQUIRE(sender.prepare().has_value());
    CY_REQUIRE(receiver.prepare().has_value());

    // Channel 3 carries a reliable message that will be lost; channel 4 carries unreliable state.
    cy::Array<u8> chat(allocator());
    CY_REQUIRE(
        sender.frame(3, DeliveryMode::ReliableOrdered, cy::Span<const u8>(kPayload, 4), 0, chat)
            .has_value());
    CY_CHECK_EQ(deliver(chat, receiver, /*drop_index=*/0), 0U);

    cy::Array<u8> state(allocator());
    for (u32 index = 0; index < 5; ++index) {
        CY_REQUIRE(
            sender.frame(4, DeliveryMode::Unreliable, cy::Span<const u8>(kPayload, 4), 0, state)
                .has_value());
    }
    CY_CHECK_EQ(deliver(state, receiver, 0xFFFF'FFFFU), 5U);

    // The requirement, as a number: channel 4 delivered everything while channel 3 is still waiting
    // for a retransmission. A single ordered stream would have delivered none of it.
    CY_CHECK_EQ(drain(receiver, 4), 5U);
    CY_CHECK_EQ(drain(receiver, 3), 0U);
    CY_CHECK_EQ(sender.channel(3).unacknowledged(), 1U);
    CY_CHECK_EQ(sender.channel(4).unacknowledged(), 0U);
}

CY_TEST_CASE("networking: a peer that never answers is abandoned rather than retried forever") {
    ReliableEndpoint sender(allocator());
    CY_REQUIRE(sender.prepare().has_value());
    RetransmitPolicy policy;
    policy.initial_timeout_ms = 10;
    policy.maximum_timeout_ms = 10;
    policy.maximum_attempts = 4;
    sender.set_policy(policy);

    cy::Array<u8> wire(allocator());
    CY_REQUIRE(
        sender.frame(0, DeliveryMode::ReliableOrdered, cy::Span<const u8>(kPayload, 4), 0, wire)
            .has_value());

    for (u64 now = 20; now <= 200; now += 20) {
        cy::Array<u8> resent(allocator());
        CY_REQUIRE(sender.collect_outgoing(now, resent).has_value());
    }
    CY_CHECK(sender.abandoned());
    CY_CHECK_EQ(sender.retransmissions(), u64{3});  // maximum_attempts, less the original send
}

CY_TEST_CASE("networking: a transport declares what it has, and a shipping build checks") {
    TransportSecurity local;
    local.in_process = true;
    local.replay_protected = true;
    local.sequence_validated = true;
    CY_CHECK(admissible_for(local, Deployment::InProcess) == SecurityRefusal::None);
    CY_CHECK(admissible_for(local, Deployment::Shipping) == SecurityRefusal::None);

    TransportSecurity udp;
    udp.replay_protected = true;
    udp.sequence_validated = true;
    CY_CHECK(admissible_for(udp, Deployment::Development) == SecurityRefusal::None);
    // "no transport SHALL be offered as a default without them", as a refusal.
    CY_CHECK(admissible_for(udp, Deployment::Shipping) == SecurityRefusal::NotEncrypted);

    TransportSecurity naive;
    CY_CHECK(admissible_for(naive, Deployment::InProcess) == SecurityRefusal::NoSequenceValidation);
    naive.sequence_validated = true;
    CY_CHECK(admissible_for(naive, Deployment::InProcess) == SecurityRefusal::NoReplayProtection);

    TransportSecurity encrypted_only;
    encrypted_only.sequence_validated = true;
    encrypted_only.replay_protected = true;
    encrypted_only.encrypted = true;
    CY_CHECK(admissible_for(encrypted_only, Deployment::Shipping) ==
             SecurityRefusal::NotAuthenticated);
}
