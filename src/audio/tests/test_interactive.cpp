// Playlists, layered stems and musical transitions. M8.b task 10.2.

#include <cy/audio/interactive.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cmath>

using namespace cy;
using namespace cy::audio;

namespace {

[[nodiscard]] MusicalClip clip_of(f32 tempo, u32 beats_per_bar, f32 seconds) noexcept {
    MusicalClip clip;
    clip.name = Name::intern("theme");
    clip.sample_rate = 48000;
    clip.tempo_bpm = tempo;
    clip.beats_per_bar = beats_per_bar;
    clip.length_samples = static_cast<u64>(static_cast<f64>(seconds) * 48000.0);
    return clip;
}

}  // namespace

CY_TEST_CASE("music_time: a beat and a bar are samples, computed from the tempo") {
    const MusicalClip clip = clip_of(120.0F, 4, 30.0F);
    // 120 beats a minute is half a second a beat: 24 000 samples.
    CY_CHECK_NEAR(static_cast<f32>(samples_per_beat(clip)), 24000.0F, 1.0F);

    // The next beat after the very start is the SECOND beat, not the one the playhead is on.
    CY_CHECK_EQ(next_transition_point(clip, 0, TransitionPoint::NextBeat), 24000U);
    CY_CHECK_EQ(next_transition_point(clip, 100, TransitionPoint::NextBeat), 24000U);
    CY_CHECK_EQ(next_transition_point(clip, 24000, TransitionPoint::NextBeat), 48000U);

    // A bar is four of those.
    CY_CHECK_EQ(next_transition_point(clip, 0, TransitionPoint::NextBar), 96000U);
    CY_CHECK_EQ(next_transition_point(clip, 100000, TransitionPoint::NextBar), 192000U);

    // Immediate is now, and end-of-clip is the end.
    CY_CHECK_EQ(next_transition_point(clip, 12345, TransitionPoint::Immediate), 12345U);
    CY_CHECK_EQ(next_transition_point(clip, 12345, TransitionPoint::EndOfClip),
                clip.length_samples);
}

CY_TEST_CASE("music_time: the next marker is the next one ahead, and the end when there is none") {
    MusicalClip clip = clip_of(120.0F, 4, 30.0F);
    const u64 markers[3] = {48000U, 240000U, 480000U};
    clip.markers = Span<const u64>(markers, 3);

    CY_CHECK_EQ(next_transition_point(clip, 0, TransitionPoint::NextMarker), 48000U);
    CY_CHECK_EQ(next_transition_point(clip, 50000, TransitionPoint::NextMarker), 240000U);
    // Past the last marker: the end of the clip is the next musically meaningful point there is.
    CY_CHECK_EQ(next_transition_point(clip, 500000, TransitionPoint::NextMarker),
                clip.length_samples);
}

CY_TEST_CASE("music_transition: nothing changes until the bar arrives, then it cross-fades") {
    // "WHEN combat begins and the transition is set to 'next bar' THEN the music SHALL switch at
    // the next bar boundary, optionally through a transition stem."
    const MusicalClip clip = clip_of(120.0F, 4, 60.0F);
    MusicTransition transition;
    transition.from = Name::intern("explore");
    transition.to = Name::intern("combat");
    transition.point = TransitionPoint::NextBar;
    transition.crossfade_seconds = 0.5F;
    CY_REQUIRE(schedule_transition(clip, 30000, transition).has_value());
    CY_CHECK_EQ(transition.at_sample, 96000U);

    // Before the bar: the source owns the mix entirely.
    const TransitionMix before = transition_mix(transition, clip, 90000);
    CY_CHECK_EQ(before.from_gain, 1.0F);
    CY_CHECK_EQ(before.to_gain, 0.0F);
    CY_CHECK_FALSE(before.complete);

    // Halfway through the cross-fade: both, at equal power.
    const TransitionMix middle = transition_mix(transition, clip, 96000 + 12000);
    CY_CHECK_NEAR(middle.from_gain, 0.707F, 0.05F);
    CY_CHECK_NEAR(middle.to_gain, 0.707F, 0.05F);
    // EQUAL POWER, not linear: the two gains square to about one, so there is no hole in the
    // middle.
    CY_CHECK_NEAR((middle.from_gain * middle.from_gain) + (middle.to_gain * middle.to_gain), 1.0F,
                  0.05F);

    const TransitionMix after = transition_mix(transition, clip, 96000 + 48000);
    CY_CHECK(after.complete);
    CY_CHECK_NEAR(after.to_gain, 1.0F, 0.01F);
}

CY_TEST_CASE("music_transition: a transition segment takes the middle") {
    const MusicalClip clip = clip_of(120.0F, 4, 60.0F);
    MusicTransition transition;
    transition.point = TransitionPoint::Immediate;
    transition.crossfade_seconds = 1.0F;
    transition.segment = Name::intern("stinger");
    CY_REQUIRE(schedule_transition(clip, 0, transition).has_value());

    const TransitionMix middle = transition_mix(transition, clip, 24000);
    CY_CHECK(middle.in_segment);
    CY_CHECK_GT(middle.segment_gain, 0.9F);
    // While the segment is playing the two stems are pulled back, so the three never pile up.
    CY_CHECK_LT(middle.from_gain, 0.2F);
    CY_CHECK_LT(middle.to_gain, 0.2F);
}

CY_TEST_CASE("music_playlist: sequential ends, loop wraps, and shuffle does not repeat") {
    const PlaylistEntry entries[3] = {PlaylistEntry{Name::intern("a"), 1.0F},
                                      PlaylistEntry{Name::intern("b"), 1.0F},
                                      PlaylistEntry{Name::intern("c"), 1.0F}};

    PlaylistState sequential;
    CY_CHECK_EQ(advance_playlist(Span<const PlaylistEntry>(entries, 3), PlaylistOrder::Sequential,
                                 sequential),
                1U);
    CY_CHECK_EQ(advance_playlist(Span<const PlaylistEntry>(entries, 3), PlaylistOrder::Sequential,
                                 sequential),
                2U);
    CY_CHECK_EQ(advance_playlist(Span<const PlaylistEntry>(entries, 3), PlaylistOrder::Sequential,
                                 sequential),
                kPlaylistEnd);
    CY_CHECK(sequential.finished);

    PlaylistState looping;
    for (u32 step = 0; step < 5U; ++step) {
        const u32 index =
            advance_playlist(Span<const PlaylistEntry>(entries, 3), PlaylistOrder::Loop, looping);
        CY_CHECK_NE(index, kPlaylistEnd);
    }

    // A SHUFFLE NEVER PLAYS THE SAME TRACK TWICE IN A ROW, which is what players notice when it
    // does.
    PlaylistState shuffling;
    u32 previous = shuffling.current;
    for (u32 step = 0; step < 20U; ++step) {
        const u32 index = advance_playlist(Span<const PlaylistEntry>(entries, 3),
                                           PlaylistOrder::Shuffle, shuffling);
        CY_CHECK_NE(index, previous);
        previous = index;
    }

    // And it is DETERMINISTIC: the same seed picks the same tracks, which a replay needs.
    PlaylistState first;
    first.seed = 12345;
    PlaylistState second;
    second.seed = 12345;
    for (u32 step = 0; step < 10U; ++step) {
        CY_CHECK_EQ(
            advance_playlist(Span<const PlaylistEntry>(entries, 3), PlaylistOrder::Shuffle, first),
            advance_playlist(Span<const PlaylistEntry>(entries, 3), PlaylistOrder::Shuffle,
                             second));
    }
}

CY_TEST_CASE("music_layers: a stem fades with the parameter, over its own time") {
    MusicLayer layers[2];
    layers[0].name = Name::intern("strings");
    layers[0].silent_at = 0.0F;
    layers[0].full_at = 1.0F;
    layers[0].fade_seconds = 0.5F;
    // A layer that fades OUT as the parameter rises is the same arithmetic with its ends swapped.
    layers[1].name = Name::intern("calm");
    layers[1].silent_at = 1.0F;
    layers[1].full_at = 0.0F;
    layers[1].fade_seconds = 0.5F;

    // Two seconds at a half-second half-life is four halvings: 0.9375 of the way there, which is
    // what a half-life MEANS. Asserting 0.95 would be asserting a different fade curve.
    for (u32 step = 0; step < 120U; ++step) {
        update_layers(Span<MusicLayer>(layers, 2), 1.0F, 1.0F / 60.0F);
    }
    CY_CHECK_GT(layers[0].gain, 0.9F);
    CY_CHECK_LT(layers[1].gain, 0.1F);

    for (u32 step = 0; step < 240U; ++step) {
        update_layers(Span<MusicLayer>(layers, 2), 0.0F, 1.0F / 60.0F);
    }
    CY_CHECK_LT(layers[0].gain, 0.01F);
    CY_CHECK_GT(layers[1].gain, 0.99F);

    // A zero fade time snaps, which is what a stinger wants.
    MusicLayer instant = layers[0];
    instant.fade_seconds = 0.0F;
    update_layers(Span<MusicLayer>(&instant, 1), 1.0F, 1.0F / 60.0F);
    CY_CHECK_EQ(instant.gain, 1.0F);
}
