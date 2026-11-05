// SPDX-License-Identifier: MIT
// ISSUE #82 — THE SECURE TRANSPORT'S PARTS, ONE AT A TIME.
//
// `unit`: arithmetic and one X25519 test vector. No socket, no session. The X25519 vector is RFC
// 7748 section 6.1's, which is the cheapest proof that the wrapper hands Monocypher its arguments
// in the right order — a swapped secret and public key still produces 32 plausible-looking bytes.

#include "fixture.h"

#include <cy/networking/congestion.h>
#include <cy/networking/crypto.h>
#include <cy/networking/fragmentation.h>
#include <cy/networking/replay_window.h>

#include <cstring>

using namespace cy::net_test;
using cy::u32;
using cy::u64;
using cy::u8;

namespace {

[[nodiscard]] cy::net::crypto::Key key_from_hex(const char* hex) noexcept {
    cy::net::crypto::Key key;
    for (u32 index = 0; index < 32; ++index) {
        auto nibble = [](char c) noexcept -> u8 {
            return static_cast<u8>(c <= '9' ? c - '0' : (c - 'a') + 10);
        };
        key.bytes[index] = static_cast<u8>((nibble(hex[static_cast<cy::usize>(index) * 2]) << 4) |
                                           nibble(hex[(static_cast<cy::usize>(index) * 2) + 1]));
    }
    return key;
}

}  // namespace

CY_TEST_CASE("secure parts: X25519 reproduces RFC 7748's test vector") {
    const auto alice =
        key_from_hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const auto alice_public =
        key_from_hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    const auto bob =
        key_from_hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    const auto bob_public =
        key_from_hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    const auto expected =
        key_from_hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");

    CY_CHECK(cy::net::crypto::public_key_of(alice) == alice_public);
    CY_CHECK(cy::net::crypto::public_key_of(bob) == bob_public);
    cy::net::crypto::Key shared_a;
    cy::net::crypto::Key shared_b;
    CY_REQUIRE(cy::net::crypto::key_exchange(alice, bob_public, shared_a));
    CY_REQUIRE(cy::net::crypto::key_exchange(bob, alice_public, shared_b));
    CY_CHECK(shared_a == expected);
    CY_CHECK(shared_b == expected);

    // A low-order point gives a shared secret every attacker knows: all zeros, refused.
    const cy::net::crypto::Key zero;
    cy::net::crypto::Key degenerate;
    CY_CHECK_FALSE(cy::net::crypto::key_exchange(alice, zero, degenerate));
}

CY_TEST_CASE("secure parts: a sealed message opens only unaltered and under its own key") {
    cy::net::crypto::SeededEntropy entropy(1);
    const auto key = cy::net::crypto::generate_key_pair(entropy.source()).secret;
    const auto other = cy::net::crypto::generate_key_pair(entropy.source()).secret;
    u8 nonce[cy::net::crypto::kNonceBytes] = {};
    nonce[0] = 7;
    const u8 header[] = {4, 0, 0, 0, 1};
    const u8 plain[] = {'a', 't', 't', 'a', 'c', 'k', ' ', 'a', 't', ' ', 'd', 'a', 'w', 'n'};
    u8 cipher[sizeof(plain)];
    u8 mac[cy::net::crypto::kMacBytes];
    cy::net::crypto::seal(key, nonce, header, plain, cipher, mac);
    CY_CHECK(std::memcmp(cipher, plain, sizeof(plain)) != 0);

    u8 opened[sizeof(plain)] = {};
    CY_CHECK(cy::net::crypto::open(key, nonce, header, cipher, mac, opened));
    CY_CHECK(std::memcmp(opened, plain, sizeof(plain)) == 0);
    CY_CHECK_FALSE(cy::net::crypto::open(other, nonce, header, cipher, mac, opened));

    cipher[3] ^= 1U;
    CY_CHECK_FALSE(cy::net::crypto::open(key, nonce, header, cipher, mac, opened));
    cipher[3] ^= 1U;
    u8 changed_header[sizeof(header)];
    std::memcpy(changed_header, header, sizeof(header));
    changed_header[4] = 2;
    CY_CHECK_FALSE(cy::net::crypto::open(key, nonce, changed_header, cipher, mac, opened));
    nonce[0] = 8;
    CY_CHECK_FALSE(cy::net::crypto::open(key, nonce, header, cipher, mac, opened));
}

CY_TEST_CASE("secure parts: the replay window refuses repeats and the too-old, nothing else") {
    cy::net::ReplayWindow window;
    CY_CHECK_FALSE(window.rejects(0));
    window.mark(0);
    CY_CHECK(window.rejects(0));
    CY_CHECK_FALSE(window.rejects(5));
    window.mark(5);
    // Out of order inside the window is fine: 1 to 4 have not been seen.
    for (u64 counter = 1; counter < 5; ++counter) {
        CY_CHECK_FALSE(window.rejects(counter));
    }
    window.mark(3);
    CY_CHECK(window.rejects(3));
    CY_CHECK_FALSE(window.rejects(4));

    // A jump past the window: everything a window behind is refused, the gap is not.
    window.mark(5000);
    CY_CHECK(window.rejects(5000 - cy::net::ReplayWindow::kWindow));
    CY_CHECK(window.rejects(4));
    CY_CHECK_FALSE(window.rejects(4999));
    CY_CHECK_FALSE(window.rejects(5000 - cy::net::ReplayWindow::kWindow + 1));

    // The bits a short jump uncovers held counters a window older; they must read as unseen.
    cy::net::ReplayWindow wrap;
    wrap.mark(10);
    wrap.mark(10 + cy::net::ReplayWindow::kWindow - 1);
    wrap.mark(10 + cy::net::ReplayWindow::kWindow + 5);
    CY_CHECK_FALSE(wrap.rejects(10 + cy::net::ReplayWindow::kWindow));
    CY_CHECK(wrap.rejects(10));
}

CY_TEST_CASE("secure parts: fragments reassemble in any order, once, and only complete") {
    cy::Array<u8> message(allocator());
    CY_REQUIRE(message.resize(3000).has_value());
    for (u32 index = 0; index < message.size(); ++index) {
        message[index] = static_cast<u8>(index * 7);
    }
    const u32 count = cy::net::fragments_for(message.size());
    CY_REQUIRE_EQ(count, 3U);

    cy::Array<u8> fragments[3] = {cy::Array<u8>(allocator()), cy::Array<u8>(allocator()),
                                  cy::Array<u8>(allocator())};
    for (u32 index = 0; index < count; ++index) {
        CY_REQUIRE(
            cy::net::write_fragment(42, index, message.span(), fragments[index]).has_value());
        CY_CHECK_LE(fragments[index].size(), cy::net::kMaxDatagramPayload);
    }
    cy::net::Reassembler reassembler(allocator(), cy::net::kMaxMessageBytes);
    cy::Span<const u8> out;
    CY_CHECK_EQ(reassembler.accept(1, fragments[2].span(), 0, out),
                cy::net::ReassemblyVerdict::Pending);
    CY_CHECK_EQ(reassembler.accept(1, fragments[0].span(), 0, out),
                cy::net::ReassemblyVerdict::Pending);
    CY_CHECK_EQ(reassembler.accept(1, fragments[0].span(), 0, out),
                cy::net::ReassemblyVerdict::Pending);  // a duplicate
    // The same message number on another channel is another message.
    CY_CHECK_EQ(reassembler.accept(2, fragments[1].span(), 0, out),
                cy::net::ReassemblyVerdict::Pending);
    CY_REQUIRE_EQ(reassembler.accept(1, fragments[1].span(), 0, out),
                  cy::net::ReassemblyVerdict::Complete);
    CY_REQUIRE_EQ(out.size(), message.size());
    CY_CHECK(std::memcmp(out.data(), message.data(), message.size()) == 0);
    CY_CHECK_EQ(reassembler.partial(), 1U);

    reassembler.expire(2'000'000, 1'000'000);
    CY_CHECK_EQ(reassembler.partial(), 0U);
    CY_CHECK_EQ(reassembler.discarded(), 1U);

    // A fragment claiming an index past its count, and a kind byte that is neither.
    fragments[0][3] = 9;
    CY_CHECK_EQ(reassembler.accept(1, fragments[0].span(), 0, out),
                cy::net::ReassemblyVerdict::Rejected);
    const u8 junk[] = {7, 1, 2};
    CY_CHECK_EQ(reassembler.accept(1, junk, 0, out), cy::net::ReassemblyVerdict::Rejected);
}

CY_TEST_CASE("secure parts: the send rate falls on loss, recovers when clean, and floors") {
    cy::net::RateConfig config;
    config.max_bytes_per_second = 100'000;
    config.min_bytes_per_second = 20'000;
    cy::net::SendRateController rate(config);
    CY_CHECK_EQ(rate.rate(), 100'000U);
    rate.on_report(300, 50'000, 40'000);
    CY_CHECK_EQ(rate.rate(), 75'000U);
    for (u32 report = 0; report < 20; ++report) {
        rate.on_report(500, 50'000, 40'000);
    }
    CY_CHECK_EQ(rate.rate(), 20'000U);
    rate.on_report(0, 50'000, 40'000);
    CY_CHECK_EQ(rate.rate(), 20'000U + 6'250U);
    // Queueing delay is congestion too, with no loss at all.
    rate.on_report(0, 400'000, 40'000);
    CY_CHECK_LT(rate.rate(), 26'250U);

    // Tokens accrue at the rate and cap at the burst.
    cy::net::SendRateController bucket(config);
    bucket.refill(0);
    CY_CHECK_FALSE(bucket.try_spend(5000));  // 40 ms of 100 KB/s is 4000
    CY_CHECK(bucket.try_spend(4000));
    CY_CHECK_FALSE(bucket.try_spend(1));
    bucket.refill(10'000);  // 10 ms: 1000 bytes
    CY_CHECK(bucket.try_spend(1000));
    CY_CHECK_FALSE(bucket.try_spend(1));
    bucket.spend_anyway(500);
    CY_CHECK_EQ(bucket.tokens(), -500);
}

CY_TEST_CASE("secure parts: round trip is smoothed and loss is counted from counters") {
    cy::net::RttEstimator rtt;
    rtt.sample(200'000);
    CY_CHECK_EQ(rtt.smoothed(), 200'000U);
    rtt.sample(280'000);
    CY_CHECK_EQ(rtt.smoothed(), 210'000U);
    CY_CHECK_EQ(rtt.minimum(), 200'000U);

    cy::net::LossMeter meter;
    for (u64 counter = 0; counter < 100; ++counter) {
        if (counter % 10 != 3) {
            meter.received(counter);
        }
    }
    CY_CHECK_EQ(meter.expected(), 100U);
    CY_CHECK_EQ(meter.received(), 90U);
    CY_CHECK_EQ(cy::net::interval_loss_per_mille(0, 0, meter.received(), meter.expected()), 100U);
    CY_CHECK_EQ(cy::net::interval_loss_per_mille(90, 100, 90, 100), 0U);
}
