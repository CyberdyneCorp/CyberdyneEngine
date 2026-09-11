// M9 TASK 3.3 — lockstep frames, the late-participant policy, the hash exchange, resynchronisation.
//
// `unit`: a log, four participant handles and integer arithmetic. No world, no session, no socket —
// which is itself part of the claim, because a lockstep frame is a fact about a session's inputs
// whether they arrived over a wire or from four local producers.
//
// THE MUTATIONS THAT PROVE THESE ARE CHECKS:
//   * make `schedule_input_delay()` accept a tick in the past — the retroactive case passes a
//   change
//     that would desync two peers.
//   * drop the `record_decision()` call from `close_frame()` — the recorded-decision case reports
//     zero `ExternalResult` records where it expects two, and the replay of that session would
//     silently re-derive the deadline instead of reproducing it.
//   * make `observe_peer_hash()` return `Agreed` on a mismatch — the divergence case goes red at
//   the
//     verdict and at `first_diverging_tick()`.

#include "fixture.h"

#include <cy/replay/lockstep.h>

using namespace cy::replay_test;
using cy::u32;
using cy::u64;
using cy::determinism::Epoch;

namespace {

constexpr u64 kAlice = 0xA11CE;
constexpr u64 kBob = 0xB0B;
constexpr u64 kCarol = 0xCA401;
constexpr u64 kDave = 0xDA7E;

[[nodiscard]] u32 count_kind(const RecordLog& log, RecordKind kind) noexcept {
    u32 found = 0;
    for (u32 index = 0; index < log.size(); ++index) {
        if (log.at(index).kind == kind) {
            ++found;
        }
    }
    return found;
}

}  // namespace

CY_TEST_CASE("replay: the input delay changes at a tick every peer computes the same way") {
    RecordLog log(allocator(), manifest());
    LockstepConfiguration configuration;
    configuration.input_delay_ticks = 2;
    LockstepSession session(allocator(), log, configuration);

    CY_CHECK_EQ(session.input_delay(0), 2U);
    CY_CHECK_EQ(session.issue_tick(100), u64{102});

    // A change applied retroactively is a desync with a stopwatch in it: two peers that received
    // the message at different ticks would apply it to different ticks.
    CY_CHECK_FALSE(session.schedule_input_delay(5, 100, 100).has_value());
    CY_CHECK_FALSE(session.schedule_input_delay(5, 90, 100).has_value());

    CY_REQUIRE(session.schedule_input_delay(5, 200, 100).has_value());
    CY_CHECK_FALSE(session.schedule_input_delay(6, 200, 100).has_value());  // that tick is taken
    CY_CHECK_EQ(session.scheduled_changes(), 1U);

    // A pure function of the schedule: the answer does not depend on when it is asked.
    CY_CHECK_EQ(session.input_delay(199), 2U);
    CY_CHECK_EQ(session.input_delay(200), 5U);
    CY_CHECK_EQ(session.input_delay(1'000'000), 5U);
    CY_CHECK_EQ(session.issue_tick(200), u64{205});

    // Two changes, delivered out of order, still resolve by tick rather than by arrival.
    CY_REQUIRE(session.schedule_input_delay(9, 400, 100).has_value());
    CY_REQUIRE(session.schedule_input_delay(7, 300, 100).has_value());
    CY_CHECK_EQ(session.input_delay(300), 7U);
    CY_CHECK_EQ(session.input_delay(399), 7U);
    CY_CHECK_EQ(session.input_delay(400), 9U);
}

CY_TEST_CASE("replay: a late participant is handled by policy and the decision is recorded") {
    RecordLog log(allocator(), manifest());
    LockstepConfiguration configuration;
    configuration.late_policy = LatePolicy::TreatAsNoCommand;
    LockstepSession session(allocator(), log, configuration);
    CY_REQUIRE(session.add_participant(kAlice).has_value());
    CY_REQUIRE(session.add_participant(kBob).has_value());
    CY_REQUIRE(session.add_participant(kCarol).has_value());
    CY_REQUIRE(session.add_participant(kDave).has_value());
    CY_CHECK_FALSE(session.add_participant(kAlice).has_value());
    CY_CHECK_FALSE(session.add_participant(0).has_value());
    CY_CHECK_EQ(session.participant_count(), 4U);

    // Tick 10: everybody on time. Nothing is recorded, because nothing was decided.
    for (u64 participant : {kAlice, kBob, kCarol, kDave}) {
        session.arrived(participant, 10);
    }
    auto full = session.close_frame(10, Epoch{});
    CY_REQUIRE(full.has_value());
    CY_CHECK_EQ(full->participants, 4U);
    CY_CHECK_EQ(full->arrived, 4U);
    CY_CHECK_EQ(full->late, 0U);
    CY_CHECK_FALSE(full->paused);
    CY_CHECK_EQ(count_kind(log, RecordKind::ExternalResult), 0U);

    // Tick 11: Carol and Dave miss the deadline. TWO DECISIONS, TWO RECORDS — the replay reproduces
    // the deadline rather than re-deriving it, which it could not do without the network.
    session.arrived(kAlice, 11);
    session.arrived(kBob, 11);
    auto late = session.close_frame(11, Epoch{});
    CY_REQUIRE(late.has_value());
    CY_CHECK_EQ(late->arrived, 2U);
    CY_CHECK_EQ(late->late, 2U);
    CY_CHECK_EQ(session.late_decisions(), 2U);
    CY_REQUIRE_EQ(count_kind(log, RecordKind::ExternalResult), 2U);

    // The record names the participant and the policy, under a source id that cannot be mistaken
    // for a bare participant handle.
    u32 seen = 0;
    for (u32 index = 0; index < log.size(); ++index) {
        const LogRecord& record = log.at(index);
        if (record.kind != RecordKind::ExternalResult) {
            continue;
        }
        ++seen;
        CY_CHECK_EQ(record.tick, u64{11});
        CY_CHECK_EQ(record.value, static_cast<u64>(LatePolicy::TreatAsNoCommand));
        u64 participant = 0;
        for (u32 byte = 0; byte < 8; ++byte) {
            participant |= static_cast<u64>(record.payload[byte]) << (byte * 8U);
        }
        CY_CHECK((participant == kCarol || participant == kDave));
        CY_CHECK_EQ(record.subject, cy::replay::late_decision_source(participant));
        CY_CHECK(record.subject != participant);
    }
    CY_CHECK_EQ(seen, 2U);

    // Frames close in order. A session that closed 11 after 12 would record its decisions out of
    // the order the session made them.
    CY_CHECK_FALSE(session.close_frame(11, Epoch{}).has_value());
    CY_CHECK_EQ(session.frames_closed(), 2U);
}

CY_TEST_CASE("replay: PauseSession does not advance the frame and RemoveParticipant does") {
    RecordLog paused_log(allocator(), manifest());
    LockstepConfiguration pausing;
    pausing.late_policy = LatePolicy::PauseSession;
    LockstepSession pausing_session(allocator(), paused_log, pausing);
    CY_REQUIRE(pausing_session.add_participant(kAlice).has_value());
    CY_REQUIRE(pausing_session.add_participant(kBob).has_value());
    pausing_session.arrived(kAlice, 4);

    auto stalled = pausing_session.close_frame(4, Epoch{});
    CY_REQUIRE(stalled.has_value());
    CY_CHECK(stalled->paused);
    // Nothing was consumed, so nothing is recorded and the frame is NOT counted as closed — a
    // retrying caller must not watch the frame count climb while the tick stands still.
    CY_CHECK_EQ(pausing_session.frames_closed(), 0U);
    CY_CHECK_EQ(paused_log.size(), 0U);

    // The same tick can be closed again once the straggler arrives.
    pausing_session.arrived(kBob, 4);
    auto resumed = pausing_session.close_frame(4, Epoch{});
    CY_REQUIRE(resumed.has_value());
    CY_CHECK_FALSE(resumed->paused);
    CY_CHECK_EQ(pausing_session.frames_closed(), 1U);

    RecordLog removal_log(allocator(), manifest());
    LockstepConfiguration removing;
    removing.late_policy = LatePolicy::RemoveParticipant;
    LockstepSession removing_session(allocator(), removal_log, removing);
    CY_REQUIRE(removing_session.add_participant(kAlice).has_value());
    CY_REQUIRE(removing_session.add_participant(kBob).has_value());
    removing_session.arrived(kAlice, 4);
    auto removed = removing_session.close_frame(4, Epoch{});
    CY_REQUIRE(removed.has_value());
    CY_CHECK_EQ(removed->late, 1U);
    CY_CHECK_EQ(removing_session.participant_count(), 1U);
    // And the next frame expects one participant rather than reporting the departed one late
    // for ever.
    removing_session.arrived(kAlice, 5);
    auto after = removing_session.close_frame(5, Epoch{});
    CY_REQUIRE(after.has_value());
    CY_CHECK_EQ(after->participants, 1U);
    CY_CHECK_EQ(after->late, 0U);
}

CY_TEST_CASE("replay: a hash exchange detects divergence rather than enduring it") {
    RecordLog log(allocator(), manifest());
    LockstepConfiguration configuration;
    configuration.hash_exchange_interval = 5;
    LockstepSession session(allocator(), log, configuration);
    CY_REQUIRE(session.add_participant(kAlice).has_value());

    CY_CHECK(session.exchange_due(0));
    CY_CHECK_FALSE(session.exchange_due(3));
    CY_CHECK(session.exchange_due(10));

    CY_REQUIRE(session.publish_hash(5, 0xAAAA, Epoch{}).has_value());
    CY_REQUIRE(session.publish_hash(10, 0xBBBB, Epoch{}).has_value());
    CY_CHECK_EQ(count_kind(log, RecordKind::StateHash), 2U);

    // A tick this peer never hashed is neither an agreement nor a divergence, and saying either
    // would be an opinion about a comparison that did not happen.
    CY_CHECK(session.observe_peer_hash(kAlice, 7, 0xAAAA) == DesyncVerdict::NotCompared);
    CY_CHECK_FALSE(session.diverged());

    CY_CHECK(session.observe_peer_hash(kAlice, 5, 0xAAAA) == DesyncVerdict::Agreed);
    u64 agreed_tick = 0;
    u64 agreed_hash = 0;
    CY_REQUIRE(session.agreement(agreed_tick, agreed_hash));
    CY_CHECK_EQ(agreed_tick, u64{5});
    CY_CHECK_EQ(agreed_hash, u64{0xAAAA});

    CY_CHECK(session.observe_peer_hash(kAlice, 10, 0xC0FFEE) == DesyncVerdict::Diverged);
    CY_CHECK(session.diverged());
    CY_CHECK_EQ(session.first_diverging_tick(), u64{10});
    CY_CHECK_EQ(session.diverging_participant(), kAlice);

    // A frame closed after the divergence still carries the last AGREED hash, which is the
    // "reference to a previously agreed state hash" a lockstep frame is defined to hold.
    session.arrived(kAlice, 11);
    auto frame = session.close_frame(11, Epoch{});
    CY_REQUIRE(frame.has_value());
    CY_CHECK(frame->has_agreement);
    CY_CHECK_EQ(frame->agreed_hash_tick, u64{5});
    CY_CHECK_EQ(frame->agreed_hash, u64{0xAAAA});
}

CY_TEST_CASE("replay: resynchronisation resets the timeline, and all three policies are recorded") {
    for (DesyncPolicy policy :
         {DesyncPolicy::Resynchronise, DesyncPolicy::Disconnect, DesyncPolicy::Continue}) {
        RecordLog log(allocator(), manifest());
        LockstepConfiguration configuration;
        configuration.desync_policy = policy;
        LockstepSession session(allocator(), log, configuration);
        cy::determinism::EpochCounter epochs;

        CY_REQUIRE(session.publish_hash(20, 0x1111, epochs.current()).has_value());
        CY_CHECK(session.observe_peer_hash(kAlice, 20, 0x2222) == DesyncVerdict::Diverged);

        ResyncReport report;
        CY_REQUIRE(session.resynchronise(epochs, 21, 0x2222, report).has_value());
        CY_CHECK(report.policy == policy);

        // THE DECISION IS RECORDED WHATEVER IT WAS. A session that decided to carry on and one that
        // never noticed read identically otherwise.
        CY_CHECK_EQ(count_kind(log, RecordKind::ExternalResult), 1U);

        if (policy == DesyncPolicy::Resynchronise) {
            CY_CHECK(report.performed);
            CY_CHECK_EQ(epochs.current().value, 1U);
            CY_CHECK(epochs.reason() == cy::determinism::EpochReason::WorldReload);
            CY_CHECK_FALSE(session.diverged());
            u64 tick = 0;
            u64 hash = 0;
            CY_REQUIRE(session.agreement(tick, hash));
            CY_CHECK_EQ(hash, u64{0x2222});
            CY_CHECK_EQ(session.resynchronisations(), 1U);
        } else {
            CY_CHECK_FALSE(report.performed);
            CY_CHECK_EQ(epochs.current().value, 0U);
            CY_CHECK(session.diverged());
            CY_CHECK_EQ(session.resynchronisations(), 0U);
        }
    }
}
