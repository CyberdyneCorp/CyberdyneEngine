// M9 TASK 4.5 — THE RECONCILIATION POLICY, AND WHAT IT DELIBERATELY DOES NOT OWN.
//
// `networking-and-replication` puts the mechanism in `replay-and-rollback` and the policy here. So
// these cases are about decisions — correct, resynchronise, or do nothing — and about the two
// bounded structures the policy owns: the input buffer and the proxy history.
//
// THE FIRST CASE IS THE MILESTONE'S SUBTITLE AS A TYPE. `InputBuffer::record_type` is
// `replay::LogRecord`, which is what makes "a second representation of participant intent SHALL NOT
// exist" a compile-time fact rather than a review note.

#include "fixture.h"

#include <cy/networking/prediction.h>
#include <cy/replay/record.h>

#include <type_traits>

using namespace cy::net_test;
using cy::u32;
using cy::u64;

namespace {

[[nodiscard]] cy::replay::LogRecord command_at(u64 tick, u32 sequence) noexcept {
    cy::replay::LogRecord record;
    record.kind = cy::replay::RecordKind::Command;
    record.tick = tick;
    record.sequence = sequence;
    record.command.tick = tick;
    record.command.sequence = sequence;
    return record;
}

}  // namespace

CY_TEST_CASE("networking: the input buffer holds THE record, not one of its own") {
    // The compile-time half. `replay::ReadsTheOneRecord` is the strong concept the five readers are
    // static_asserted against; the input buffer holds the same type for the same reason, and
    // changing it to a record of networking's own stops this translation unit compiling.
    static_assert(cy::replay::ReadsTheOneRecord<InputBuffer>,
                  "the client's unacknowledged inputs are participant intent, and there is one "
                  "representation of participant intent");
    static_assert(std::is_same_v<InputBuffer::record_type, cy::replay::LogRecord>);

    InputBuffer buffer(allocator());
    CY_REQUIRE(buffer.retain(command_at(10, 0)).has_value());
    CY_REQUIRE(buffer.retain(command_at(10, 1)).has_value());
    CY_REQUIRE(buffer.retain(command_at(11, 0)).has_value());
    CY_CHECK_EQ(buffer.size(), 3U);
    CY_CHECK_EQ(buffer.oldest_tick(), u64{10});
    CY_CHECK_EQ(buffer.newest_tick(), u64{11});

    // A state hash in an input buffer would be re-simulated as intent.
    cy::replay::LogRecord hash;
    hash.kind = cy::replay::RecordKind::StateHash;
    hash.tick = 12;
    CY_CHECK_FALSE(buffer.retain(hash).has_value());
}

CY_TEST_CASE("networking: an acknowledgement drops what it covers and nothing after it") {
    InputBuffer buffer(allocator());
    for (u64 tick = 1; tick <= 6; ++tick) {
        CY_REQUIRE(buffer.retain(command_at(tick, 0)).has_value());
    }
    CY_CHECK_EQ(buffer.acknowledge(3), 3U);
    CY_CHECK_EQ(buffer.size(), 3U);
    CY_CHECK_EQ(buffer.oldest_tick(), u64{4});
    CY_CHECK_EQ(buffer.acknowledged_through(), u64{3});

    // Retaining an input the authority has already applied would replay it.
    CY_CHECK_FALSE(buffer.retain(command_at(2, 0)).has_value());

    cy::Array<cy::replay::LogRecord> range(allocator());
    CY_REQUIRE(buffer.replay_range(4, 5, range).has_value());
    CY_CHECK_EQ(range.size(), cy::usize{2});
    CY_CHECK_EQ(range[0].tick, u64{4});
    CY_CHECK_EQ(range[1].tick, u64{5});
    CY_CHECK_FALSE(buffer.replay_range(5, 4, range).has_value());
}

CY_TEST_CASE("networking: prediction matching, correcting, and falling outside the window") {
    PredictionLedger ledger(allocator());
    ReconciliationPolicy policy;
    policy.window_ticks = 8;
    policy.tolerance = 0;
    ledger.set_policy(policy);

    for (u64 tick = 1; tick <= 12; ++tick) {
        CY_REQUIRE(ledger.predict(tick, 0x1000 + tick).has_value());
    }
    // Bounded: the window is trimmed as it moves, not when someone remembers.
    CY_CHECK_LE(ledger.retained(), 9U);

    CY_CHECK(ledger.compare(12, 0x1000 + 12, 0) == ReconciliationVerdict::Matched);
    CY_CHECK(ledger.compare(11, 0xDEAD, 250) == ReconciliationVerdict::Correct);
    CY_CHECK(ledger.compare(2, 0xBEEF, 1) == ReconciliationVerdict::Resynchronise);
    // A tick inside the window that was never predicted is its own answer: reporting it as matched
    // would make the success rate meaningless.
    CY_CHECK(ledger.compare(40, 0x1234, 1) == ReconciliationVerdict::NotPredicted);

    CY_CHECK_EQ(ledger.report().compared, u64{4});
    CY_CHECK_EQ(ledger.report().matched, u64{1});
    CY_CHECK_EQ(ledger.report().corrected, u64{1});
    CY_CHECK_EQ(ledger.report().resynchronised, u64{1});
    CY_CHECK_EQ(ledger.report().not_predicted, u64{1});
    CY_CHECK_EQ(ledger.report().largest_error, u64{250});
    CY_CHECK_EQ(ledger.report().last_corrected_tick, u64{11});
}

CY_TEST_CASE("networking: a difference inside the tolerance is not a correction") {
    PredictionLedger ledger(allocator());
    ReconciliationPolicy policy;
    policy.window_ticks = 16;
    policy.tolerance = 100;
    ledger.set_policy(policy);
    CY_REQUIRE(ledger.predict(5, 0xAAAA).has_value());

    CY_CHECK(ledger.compare(5, 0xBBBB, 50) == ReconciliationVerdict::Matched);
    CY_CHECK(ledger.compare(5, 0xBBBB, 101) == ReconciliationVerdict::Correct);
}

CY_TEST_CASE("networking: a correction converges rather than snapping") {
    CorrectionSmoother smoother;
    smoother.begin(100, /*smoothing_ticks=*/8);
    CY_CHECK_EQ(smoother.weight_at(100), 0U);
    CY_CHECK_EQ(smoother.weight_at(104), 50U);
    CY_CHECK_EQ(smoother.weight_at(108), 100U);
    CY_CHECK(smoother.active(103));
    CY_CHECK_FALSE(smoother.active(108));
    CY_CHECK_EQ(smoother.corrections(), u64{1});

    // Zero smoothing is a snap, and is reported as one rather than as a division by zero.
    CorrectionSmoother immediate;
    immediate.begin(100, 0);
    CY_CHECK_EQ(immediate.weight_at(100), 100U);
    CY_CHECK_FALSE(immediate.active(100));
}

CY_TEST_CASE("networking: a rewind is bounded by the window and by the claimant's latency") {
    ProxyHistory history(allocator(), /*window_ticks=*/16);
    NetworkIdMinter minter(1);
    const NetworkId target = minter.mint();

    for (u64 tick = 1; tick <= 40; ++tick) {
        const CollisionProxy proxies[2] = {
            {target, static_cast<cy::i64>(tick) * 10, 0, 0, 50},
            {minter.mint(), 0, static_cast<cy::i64>(tick), 0, 50},
        };
        CY_REQUIRE(history.record(tick, cy::Span<const CollisionProxy>(proxies, 2)).has_value());
    }
    CY_CHECK_EQ(history.recorded_ticks(), u64{40});

    cy::Array<CollisionProxy> rewound(allocator());
    // A client three ticks behind, claiming three ticks back. Plausible, inside the window.
    CY_CHECK(history.rewind(37, 40, /*peer_latency_ticks=*/3, rewound) == RewindRefusal::None);
    CY_CHECK_EQ(rewound.size(), cy::usize{2});
    CY_CHECK_EQ(rewound[0].x, cy::i64{370});

    rewound.clear();
    // The same claim from a client with no measured latency: refused, which is what bounds the
    // advantage a high-latency or malicious client can obtain.
    CY_CHECK(history.rewind(37, 40, 0, rewound) == RewindRefusal::ImplausibleForLatency);
    CY_CHECK(history.rewind(20, 40, 100, rewound) == RewindRefusal::OutsideWindow);
    CY_CHECK(history.rewind(41, 40, 100, rewound) == RewindRefusal::InTheFuture);
    CY_CHECK_EQ(history.refusals(), u64{3});

    // The accuracy difference is documented at the call site rather than in a design document.
    CY_CHECK(kProxyAccuracyNote[0] != '\0');
}
