// M9 TASK 1.2 — external results: recorded, consumed, and the source not invoked.
//
// The claim "the external source SHALL NOT be invoked" is a negative, and a counter is how a
// negative is checked. `produce_invocations()` is zero after a replay, and the producer used below
// would fail the test loudly if it were ever called during one.

#include "fixture.h"

#include <cy/replay/external.h>

#include <cstring>

using namespace cy::replay_test;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::determinism::Epoch;
using cy::determinism::SimulationPoint;

namespace {

constexpr u64 kMatchmaking = 0x4A7C4;
constexpr u64 kInference = 0x1FE7;

struct Service {
    u32 calls = 0;
    u8 next = 0;
};

cy::Status answer(void* user, cy::Array<u8>& out) noexcept {
    auto* service = static_cast<Service*>(user);
    ++service->calls;
    return out.push_back(service->next++);
}

/// A producer a replay must never reach. If it is called, the value it returns is wrong in a way
/// the assertions below catch, and its own counter says so.
cy::Status must_not_run(void* user, cy::Array<u8>& out) noexcept {
    auto* service = static_cast<Service*>(user);
    ++service->calls;
    return out.push_back(0xFFU);
}

void declare_sources(ExternalResults& results) noexcept {
    CY_REQUIRE(results.declare({kMatchmaking, "matchmaking", ExternalKind::MatchmakingAssignment})
                   .has_value());
    CY_REQUIRE(results.declare({kInference, "inference", ExternalKind::Inference}).has_value());
}

}  // namespace

CY_TEST_CASE("replay: an inference result is recorded and replay uses the record, not the model") {
    // "WHEN an inference result influences authoritative gameplay THEN it SHALL be recorded, and
    // replay SHALL use the recorded value rather than running the model again."
    RecordLog log(allocator(), manifest());
    Service service;
    cy::Array<u8> value(allocator());

    ExternalResults recording(allocator(), ExternalMode::Recording);
    declare_sources(recording);
    for (u64 tick = 0; tick < 4; ++tick) {
        CY_REQUIRE(
            recording
                .consume(kInference, SimulationPoint{Epoch{1}, tick}, &answer, &service, log, value)
                .has_value());
        CY_REQUIRE_EQ(value.size(), cy::usize{1});
        CY_CHECK_EQ(value[0], static_cast<u8>(tick));
    }
    CY_CHECK_EQ(recording.produce_invocations(), 4U);
    CY_CHECK_EQ(recording.report().recorded, 4U);
    CY_CHECK_EQ(log.size(), 4U);

    // The replay side. A *different* epoch, because a rollback advances it and the recorded value
    // for the tick is still the value that tick consumed.
    Service never;
    ExternalResults replaying(allocator(), ExternalMode::Replaying);
    declare_sources(replaying);
    CY_REQUIRE(replaying.adopt(log).has_value());
    CY_CHECK_EQ(replaying.report().recorded, 4U);
    for (u64 tick = 0; tick < 4; ++tick) {
        CY_REQUIRE(replaying
                       .consume(kInference, SimulationPoint{Epoch{9}, tick}, &must_not_run, &never,
                                log, value)
                       .has_value());
        CY_REQUIRE_EQ(value.size(), cy::usize{1});
        CY_CHECK_EQ(value[0], static_cast<u8>(tick));
    }
    // THE NEGATIVE, AS A NUMBER. Not invoked and ignored; not invoked and compared. Not invoked.
    CY_CHECK_EQ(replaying.produce_invocations(), 0U);
    CY_CHECK_EQ(never.calls, 0U);
    CY_CHECK_EQ(replaying.finish().unconsumed_records, 0U);
}

CY_TEST_CASE("replay: a consumption with no record is reported, not silently diverged past") {
    // "WHEN a system consumes an external source that was not recorded THEN validation SHALL report
    // it rather than the replay silently diverging."
    RecordLog log(allocator(), manifest());
    Service service;
    cy::Array<u8> value(allocator());

    ExternalResults recording(allocator(), ExternalMode::Recording);
    declare_sources(recording);
    // Only the inference was recorded. The matchmaking assignment was consumed by a system that
    // never declared it to the recording — the exact defect the requirement is about.
    CY_REQUIRE(
        recording.consume(kInference, SimulationPoint{Epoch{1}, 0}, &answer, &service, log, value)
            .has_value());

    Service never;
    ExternalResults replaying(allocator(), ExternalMode::Replaying);
    declare_sources(replaying);
    CY_REQUIRE(replaying.adopt(log).has_value());
    CY_REQUIRE(
        replaying
            .consume(kInference, SimulationPoint{Epoch{1}, 0}, &must_not_run, &never, log, value)
            .has_value());
    CY_CHECK_FALSE(
        replaying
            .consume(kMatchmaking, SimulationPoint{Epoch{1}, 0}, &must_not_run, &never, log, value)
            .has_value());

    CY_CHECK_EQ(replaying.report().unrecorded_consumptions, 1U);
    CY_CHECK(std::strcmp(replaying.report().first_unrecorded, "matchmaking") == 0);
    // And still not invoked: a missing record is refused rather than papered over by calling the
    // service, which would make the replay depend on the service being up.
    CY_CHECK_EQ(never.calls, 0U);
}

CY_TEST_CASE("replay: an undeclared source is refused in both modes") {
    RecordLog log(allocator(), manifest());
    Service service;
    cy::Array<u8> value(allocator());

    ExternalResults recording(allocator(), ExternalMode::Recording);
    CY_CHECK_FALSE(
        recording.consume(kInference, SimulationPoint{Epoch{1}, 0}, &answer, &service, log, value)
            .has_value());
    CY_CHECK_EQ(service.calls, 0U);

    ExternalResults replaying(allocator(), ExternalMode::Replaying);
    CY_CHECK_FALSE(
        replaying.consume(kInference, SimulationPoint{Epoch{1}, 0}, &answer, &service, log, value)
            .has_value());

    // Declaring twice, and declaring the null identity, are both refused.
    CY_REQUIRE(recording.declare({kInference, "inference", ExternalKind::Inference}).has_value());
    CY_CHECK_FALSE(recording.declare({kInference, "again", ExternalKind::Inference}).has_value());
    CY_CHECK_FALSE(recording.declare({0, "null", ExternalKind::Other}).has_value());
}

CY_TEST_CASE("replay: a replay that stopped early says how much it did not consume") {
    RecordLog log(allocator(), manifest());
    Service service;
    cy::Array<u8> value(allocator());

    ExternalResults recording(allocator(), ExternalMode::Recording);
    declare_sources(recording);
    for (u64 tick = 0; tick < 5; ++tick) {
        CY_REQUIRE(
            recording
                .consume(kInference, SimulationPoint{Epoch{1}, tick}, &answer, &service, log, value)
                .has_value());
    }

    Service never;
    ExternalResults replaying(allocator(), ExternalMode::Replaying);
    declare_sources(replaying);
    CY_REQUIRE(replaying.adopt(log).has_value());
    for (u64 tick = 0; tick < 2; ++tick) {
        CY_REQUIRE(replaying
                       .consume(kInference, SimulationPoint{Epoch{1}, tick}, &must_not_run, &never,
                                log, value)
                       .has_value());
    }
    // Not an error — a replay may stop early — and reported, because a replay that consumed a fifth
    // of what was recorded is a replay that diverged somewhere.
    CY_CHECK_EQ(replaying.finish().unconsumed_records, 3U);
    CY_CHECK_EQ(replaying.report().consumed, 2U);
}

CY_TEST_CASE("replay: an external result longer than a payload is refused as content") {
    RecordLog log(allocator(), manifest());
    cy::Array<u8> value(allocator());

    struct Flood {
        static cy::Status run(void* /*user*/, cy::Array<u8>& out) noexcept {
            for (u32 index = 0; index < kMaxRecordPayload + 1; ++index) {
                if (cy::Status pushed = out.push_back(static_cast<u8>(index)); !pushed) {
                    return pushed;
                }
            }
            return cy::ok();
        }
    };

    ExternalResults recording(allocator(), ExternalMode::Recording);
    declare_sources(recording);
    CY_CHECK_FALSE(
        recording
            .consume(kInference, SimulationPoint{Epoch{1}, 0}, &Flood::run, nullptr, log, value)
            .has_value());
    CY_CHECK_EQ(log.size(), 0U);
}
