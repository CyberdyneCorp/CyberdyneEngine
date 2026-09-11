// M9 TASK 3.1 — the parts of playback that are arithmetic: the clock, the tracks, the ring sizing.
//
// `unit`: no world, no session, no device. Everything here is integers and a small array.
//
// THE MUTATIONS THAT PROVE THESE ARE CHECKS:
//   * put `speed_` into `PlaybackClock::tick_micros()` — "the step does not change with speed" goes
//     red naming both numbers.
//   * make `PlaybackClock::advance()` clear the accumulator instead of carrying the remainder — the
//     hundred-calls case reports 0 ticks, because every call is shorter than one tick.
//   * make `PresentationTrack::append()` sort instead of refusing — the monotonicity case passes a
//     sample it should have refused.

#include "fixture.h"

#include <cy/replay/crash.h>
#include <cy/replay/playback.h>

using namespace cy::replay_test;
using cy::u32;
using cy::u64;

CY_TEST_CASE("replay: playback speed changes what is presented and never the step") {
    PlaybackClock clock(60, 1);
    CY_REQUIRE(clock.valid());

    const u64 step = clock.tick_micros();
    CY_CHECK_EQ(step, u64{16666});

    // ONE SECOND, DELIVERED IN A HUNDRED PIECES OF TEN MILLISECONDS. Each piece is SHORTER than a
    // tick, so a clock that divided per call and threw the remainder away would issue zero ticks
    // for all hundred of them. Carrying the remainder issues exactly sixty.
    u32 ticks = 0;
    for (u32 call = 0; call < 100; ++call) {
        ticks += clock.advance(10'000);
    }
    CY_CHECK_EQ(ticks, 60U);

    // A TENTH SPEED. The same wall time yields a tenth of the ticks, and the step is the same
    // number it was — `tick_micros()` is a function of the tick rate alone.
    CY_REQUIRE(clock.set_speed(PlaybackSpeed{1, 10}).has_value());
    CY_CHECK_EQ(clock.tick_micros(), step);
    clock.reset();
    u32 slow = 0;
    for (u32 call = 0; call < 100; ++call) {
        slow += clock.advance(10'000);
    }
    CY_CHECK_EQ(slow, 6U);

    // Four times speed, same again.
    CY_REQUIRE(clock.set_speed(PlaybackSpeed{4, 1}).has_value());
    CY_CHECK_EQ(clock.tick_micros(), step);
    clock.reset();
    u32 fast = 0;
    for (u32 call = 0; call < 100; ++call) {
        fast += clock.advance(10'000);
    }
    CY_CHECK_EQ(fast, 240U);

    // A speed of zero is a pause, and a pause is a different mechanism.
    CY_CHECK_FALSE(clock.set_speed(PlaybackSpeed{0, 1}).has_value());
    CY_CHECK_FALSE(clock.set_speed(PlaybackSpeed{1, 0}).has_value());
    clock.pause();
    CY_CHECK_EQ(clock.advance(1'000'000), 0U);
    clock.resume();
    CY_CHECK(clock.advance(1'000'000) > 0);
}

CY_TEST_CASE("replay: a presentation track is tick-monotonic and answers what is in force") {
    PresentationTrackSet tracks(allocator());
    auto camera = tracks.add(PresentationTrackKind::CameraDirection, "director");
    CY_REQUIRE(camera.has_value());
    // A duplicate name would make `find()` depend on which was added first.
    CY_CHECK_FALSE(tracks.add(PresentationTrackKind::Marker, "director").has_value());
    CY_CHECK_FALSE(tracks.add(PresentationTrackKind::Marker, "").has_value());

    PresentationSample sample;
    sample.tick = 10;
    sample.subject = 7;
    CY_REQUIRE((*camera)->append(sample).has_value());
    sample.tick = 40;
    sample.subject = 9;
    CY_REQUIRE((*camera)->append(sample).has_value());
    sample.tick = 20;
    CY_CHECK_FALSE((*camera)->append(sample).has_value());  // backwards, refused not sorted
    CY_CHECK_EQ((*camera)->size(), 2U);

    PresentationSample found;
    // Before the first sample the track has not started. A replay whose direction begins half-way
    // through is legal, and "no answer" is the honest one.
    CY_CHECK_FALSE((*camera)->sample_at(9, found));
    CY_REQUIRE((*camera)->sample_at(10, found));
    CY_CHECK_EQ(found.subject, u64{7});
    CY_REQUIRE((*camera)->sample_at(39, found));
    CY_CHECK_EQ(found.subject, u64{7});
    CY_REQUIRE((*camera)->sample_at(4000, found));
    CY_CHECK_EQ(found.subject, u64{9});

    // The set answers for cameras specifically, and a marker track is not a camera.
    PresentationSample directed;
    CY_REQUIRE(tracks.camera_override(50, directed));
    CY_CHECK_EQ(directed.subject, u64{9});

    PresentationTrackSet markers_only(allocator());
    auto marker = markers_only.add(PresentationTrackKind::Marker, "chapters");
    CY_REQUIRE(marker.has_value());
    CY_REQUIRE((*marker)->append(PresentationSample{5, 1, 0, {}}).has_value());
    CY_CHECK_FALSE(markers_only.camera_override(50, directed));
}

CY_TEST_CASE("replay: the crash ring is sized from a duration and refuses what it cannot serve") {
    // Ten seconds at 60 Hz with four commands and a hash every tick.
    CY_CHECK_EQ(cy::replay::crash_ring_records(10, 60, 1, 5), 3000U);
    // Thirty seconds at 30 Hz with one record a tick.
    CY_CHECK_EQ(cy::replay::crash_ring_records(30, 30, 1, 1), 900U);
    // Nothing that cannot be served becomes a default: zero means "do not enable the buffer".
    CY_CHECK_EQ(cy::replay::crash_ring_records(0, 60, 1, 5), 0U);
    CY_CHECK_EQ(cy::replay::crash_ring_records(10, 0, 1, 5), 0U);
    CY_CHECK_EQ(cy::replay::crash_ring_records(10, 60, 0, 5), 0U);
    CY_CHECK_EQ(cy::replay::crash_ring_records(10, 60, 1, 0), 0U);
    // Bounded rather than wrapped: a ring of four billion records is not a bounded buffer.
    CY_CHECK_EQ(cy::replay::crash_ring_records(1'000'000'000ULL, 60, 1, 100), 0x0100'0000U);
}
