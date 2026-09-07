// Deadline propagation. M6 task 4.1.
//
// `residency` — "Deadline propagation", and its three scenarios: "One prediction, many
// preparations", "A camera cut is announced", and "A shot list is a schedule". The requirement
// under them is the one this file exists to hold: "Subsystems SHALL NOT each derive the same
// prediction independently, since independent derivations disagree about how soon."

#include <cy/servers/residency/deadline.h>
#include <cy/test/test.h>

using namespace cy::residency;
using cy::u32;

CY_TEST_CASE("one prediction becomes one deadline per consumer, all agreeing about how soon") {
    DeadlineBus bus;
    Prediction arrival;
    arrival.source = PredictionSource::WorldCell;
    arrival.region = 1234;
    arrival.seconds_until = 2.5;
    arrival.confidence = 0.8F;
    arrival.consumers = kAllSubsystems;
    CY_REQUIRE(bus.announce(arrival, 10.0));

    CY_CHECK_EQ(bus.size(), kSubsystemCount);

    Deadline written[kSubsystemCount];
    const u32 count = bus.collect(written, kSubsystemCount);
    CY_REQUIRE_EQ(count, kSubsystemCount);
    for (u32 index = 0; index < count; ++index) {
        // THE POINT OF THE REQUIREMENT, AS A NUMBER: every consumer's due time is the same double.
        CY_CHECK_NEAR(written[index].due_seconds, 12.5, 1e-9);
        CY_CHECK_NEAR(written[index].confidence, 0.8F, 1e-6F);
    }
    CY_CHECK_NEAR(bus.seconds_until(Subsystem::Audio, 1234, 10.0), 2.5, 1e-9);
    CY_CHECK_NEAR(bus.seconds_until(Subsystem::Geometry, 1234, 10.0), 2.5, 1e-9);
}

CY_TEST_CASE("a narrower mask prepares fewer consumers") {
    DeadlineBus bus;
    Prediction pre_roll;
    pre_roll.source = PredictionSource::Sequence;
    pre_roll.region = 7;
    pre_roll.seconds_until = 1.0;
    pre_roll.consumers = subsystem_bit(Subsystem::Audio);
    CY_REQUIRE(bus.announce(pre_roll, 0.0));

    CY_CHECK_EQ(bus.size(), 1U);
    CY_CHECK_NEAR(bus.seconds_until(Subsystem::Audio, 7, 0.0), 1.0, 1e-9);
    CY_CHECK_NEAR(bus.seconds_until(Subsystem::Texture, 7, 0.0), kNoDeadline, 1.0);
}

CY_TEST_CASE("a camera cut is announced: the earlier prediction wins") {
    DeadlineBus bus;
    Prediction extrapolated;
    extrapolated.source = PredictionSource::CameraMotion;
    extrapolated.region = 3;
    extrapolated.seconds_until = 4.0;
    extrapolated.confidence = 0.4F;
    CY_REQUIRE(bus.announce(extrapolated, 0.0));

    // A compiled sequence knows exactly where its camera will be. It says sooner, and sooner wins.
    Prediction cut;
    cut.source = PredictionSource::Sequence;
    cut.region = 3;
    cut.seconds_until = 0.2;
    cut.confidence = 1.0F;
    CY_REQUIRE(bus.announce(cut, 0.0));
    CY_CHECK_NEAR(bus.seconds_until(Subsystem::Texture, 3, 0.0), 0.2, 1e-9);

    // And a later prediction does not push the deadline back: preparing early costs memory,
    // preparing late costs a frame.
    Prediction relaxed = extrapolated;
    relaxed.seconds_until = 9.0;
    CY_REQUIRE(bus.announce(relaxed, 0.0));
    CY_CHECK_NEAR(bus.seconds_until(Subsystem::Texture, 3, 0.0), 0.2, 1e-9);
}

CY_TEST_CASE("a deadline met and a deadline missed are counted apart, per consumer") {
    DeadlineBus bus;
    Prediction arrival;
    arrival.region = 5;
    arrival.seconds_until = 1.0;
    arrival.consumers = subsystem_bit(Subsystem::Geometry) | subsystem_bit(Subsystem::Texture);
    CY_REQUIRE(bus.announce(arrival, 0.0));

    CY_CHECK(bus.satisfy(Subsystem::Geometry, 5, 0.5));  // in time
    CY_CHECK_EQ(bus.advance(2.0), 1U);                   // texture did not make it
    CY_CHECK_EQ(bus.misses(Subsystem::Texture), 1U);
    CY_CHECK_EQ(bus.misses(Subsystem::Geometry), 0U);

    const PredictionAccuracy accuracy = bus.accuracy();
    CY_CHECK_EQ(accuracy.deadlines_set, 2U);
    CY_CHECK_EQ(accuracy.deadlines_met, 1U);
    CY_CHECK_EQ(accuracy.deadlines_missed, 1U);
    CY_CHECK_NEAR(accuracy.hit_rate(), 0.5, 1e-9);

    // Advancing again does not count the same miss twice.
    CY_CHECK_EQ(bus.advance(3.0), 0U);
    CY_CHECK_EQ(bus.misses(Subsystem::Texture), 1U);
}

CY_TEST_CASE("prediction quality is measurable: prefetched against sampled") {
    DeadlineBus bus;
    bus.record_prefetched(PredictionSource::CameraMotion, 100);
    bus.record_sampled(PredictionSource::CameraMotion, 25);
    bus.record_prefetched(PredictionSource::Sequence, 10);
    bus.record_sampled(PredictionSource::Sequence, 10);

    CY_CHECK_NEAR(bus.accuracy(PredictionSource::CameraMotion).sample_rate(), 0.25, 1e-9);
    // A shot list knows what it needs; extrapolation guesses. The numbers should say so.
    CY_CHECK_NEAR(bus.accuracy(PredictionSource::Sequence).sample_rate(), 1.0, 1e-9);
    // A predictor that made no claim has not made a wrong one.
    CY_CHECK_NEAR(bus.accuracy(PredictionSource::Teleport).sample_rate(), 1.0, 1e-9);
}

CY_TEST_CASE("resolved deadlines retire and the outstanding ones stay addressable") {
    DeadlineBus bus;
    Prediction first;
    first.region = 1;
    first.seconds_until = 1.0;
    first.consumers = subsystem_bit(Subsystem::Geometry);
    Prediction second = first;
    second.region = 2;
    CY_REQUIRE(bus.announce(first, 0.0));
    CY_REQUIRE(bus.announce(second, 0.0));

    CY_CHECK(bus.satisfy(Subsystem::Geometry, 1, 0.5));
    bus.retire_resolved();

    CY_CHECK_EQ(bus.size(), 1U);
    CY_CHECK(bus.find(Subsystem::Geometry, 1) == nullptr);
    CY_REQUIRE(bus.find(Subsystem::Geometry, 2) != nullptr);
    CY_CHECK_NEAR(bus.seconds_until(Subsystem::Geometry, 2, 0.0), 1.0, 1e-9);
    CY_CHECK_EQ(bus.outstanding(Subsystem::Geometry), 1U);
}
