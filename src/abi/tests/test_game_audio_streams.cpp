// SPDX-License-Identifier: MIT
// ABI 1.18's streamed-track thunks, against a fake `AudioBackend`. Issue #87.
//
// The boundary only: argument checks, `struct_size` in both directions, the resimulation drop, and
// UNAVAILABLE from a backend that predates streams. What a real stream does is
// integration.game_backend_audio's and integration.audio_streaming's.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/audio.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <string>

namespace {

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

constexpr CyAudioTrack kTheme = 1;
constexpr CyAudioStream kStream = 41;

/// A backend that plays cues not at all and streams from a one-track catalogue.
class FakeStreams final : public cy::abi::game::AudioBackend {
public:
    CyResult find_cue(const char*, CyAudioCue&) noexcept override { return CY_RESULT_NOT_FOUND; }
    CyResult play(const CyAudioPlay&, CyAudioVoice&) noexcept override { return CY_RESULT_OK; }
    CyResult stop(CyAudioVoice, cy::f32) noexcept override { return CY_RESULT_OK; }
    bool playing(CyAudioVoice) const noexcept override { return false; }
    CyResult find_bus(const char*, CyAudioBus&) noexcept override { return CY_RESULT_NOT_FOUND; }
    CyResult set_bus_volume(CyAudioBus, cy::f32, cy::f32) noexcept override { return CY_RESULT_OK; }

    CyResult find_track(const char* name, CyAudioTrack& out_track) noexcept override {
        ++calls;
        if (std::string(name) != "theme") {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no such track");
        }
        out_track = kTheme;
        return CY_RESULT_OK;
    }
    CyResult stream_play(const CyAudioStreamPlay& request, CyAudioStream& out) noexcept override {
        ++calls;
        last_play = request;
        out = kStream;
        return CY_RESULT_OK;
    }
    CyResult stream_stop(CyAudioStream stream, cy::f32 fade) noexcept override {
        ++calls;
        stopped = stream;
        stop_fade = fade;
        return CY_RESULT_OK;
    }
    CyResult stream_seek(CyAudioStream stream, cy::f64 seconds) noexcept override {
        ++calls;
        return stream == kStream ? (sought = seconds, CY_RESULT_OK)
                                 : cy::abi::report(CY_RESULT_NOT_FOUND, "ended");
    }
    CyResult stream_set_volume(CyAudioStream, cy::f32 volume, cy::f32) noexcept override {
        ++calls;
        set_volume = volume;
        return CY_RESULT_OK;
    }
    CyResult stream_state(CyAudioStream stream, CyAudioStreamState& out) noexcept override {
        ++calls;
        if (stream != kStream) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "ended");
        }
        out.flags = CY_AUDIO_STREAM_PLAYING | CY_AUDIO_STREAM_LOOPING;
        out.position_seconds = 12.5;
        out.duration_seconds = 32.0;
        out.gain = 0.75F;
        out.underruns = 2;
        return CY_RESULT_OK;
    }

    int calls = 0;
    CyAudioStreamPlay last_play{};
    CyAudioStream stopped = 0;
    cy::f32 stop_fade = -1.0F;
    cy::f64 sought = -1.0;
    cy::f32 set_volume = -1.0F;
};

/// The 1.3 entries only: what a backend written before 1.18 looks like.
class CuesOnly final : public cy::abi::game::AudioBackend {
public:
    CyResult find_cue(const char*, CyAudioCue&) noexcept override { return CY_RESULT_NOT_FOUND; }
    CyResult play(const CyAudioPlay&, CyAudioVoice&) noexcept override { return CY_RESULT_OK; }
    CyResult stop(CyAudioVoice, cy::f32) noexcept override { return CY_RESULT_OK; }
    bool playing(CyAudioVoice) const noexcept override { return false; }
    CyResult find_bus(const char*, CyAudioBus&) noexcept override { return CY_RESULT_NOT_FOUND; }
    CyResult set_bus_volume(CyAudioBus, cy::f32, cy::f32) noexcept override { return CY_RESULT_OK; }
};

template <class Backend>
struct StreamHost {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    Backend audio;

    StreamHost() { host.game.audio = &audio; }
};

[[nodiscard]] CyAudioStreamPlay theme() noexcept {
    CyAudioStreamPlay request{};
    request.struct_size = sizeof(CyAudioStreamPlay);
    request.track = kTheme;
    request.volume = 0.8F;
    request.fade_seconds = 2.0F;
    request.crossfade_from = 7;
    request.start_seconds = 4.0;
    return request;
}

}  // namespace

CY_TEST_CASE("audio streams: a track resolves by name and plays whole, crossfade included") {
    StreamHost<FakeStreams> fixture;
    CyAudioTrack track = 0;
    CY_CHECK_EQ(table().audio_find_track(&fixture.host, "theme", &track), CY_RESULT_OK);
    CY_CHECK_EQ(track, kTheme);
    CY_CHECK_EQ(table().audio_find_track(&fixture.host, "nope", &track), CY_RESULT_NOT_FOUND);

    const CyAudioStreamPlay request = theme();
    CyAudioStream stream = 0;
    CY_CHECK_EQ(table().audio_stream_play(&fixture.host, &request, &stream), CY_RESULT_OK);
    CY_CHECK_EQ(stream, kStream);
    CY_CHECK_EQ(fixture.audio.last_play.crossfade_from, 7U);
    CY_CHECK_EQ(fixture.audio.last_play.fade_seconds, 2.0F);
    CY_CHECK_EQ(fixture.audio.last_play.start_seconds, 4.0);

    // A caller compiled before a later minor grew the struct: the prefix it knows is read and the
    // rest is zero. Here, one that stops before `start_seconds`.
    CyAudioStreamPlay short_request = theme();
    short_request.struct_size = static_cast<uint32_t>(offsetof(CyAudioStreamPlay, start_seconds));
    CY_CHECK_EQ(table().audio_stream_play(&fixture.host, &short_request, &stream), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.audio.last_play.start_seconds, 0.0);
}

CY_TEST_CASE("audio streams: a bad number is refused before the backend sees it") {
    StreamHost<FakeStreams> fixture;
    CyAudioStream stream = 0;
    for (const auto& spoil : {+[](CyAudioStreamPlay& r) { r.volume = -1.0F; },
                              +[](CyAudioStreamPlay& r) { r.fade_seconds = NAN; },
                              +[](CyAudioStreamPlay& r) { r.start_seconds = -0.5; },
                              +[](CyAudioStreamPlay& r) { r.struct_size = 3; }}) {
        CyAudioStreamPlay request = theme();
        spoil(request);
        CY_CHECK_EQ(table().audio_stream_play(&fixture.host, &request, &stream),
                    CY_RESULT_INVALID_ARGUMENT);
    }
    CY_CHECK_EQ(table().audio_stream_play(&fixture.host, nullptr, &stream),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().audio_stream_stop(&fixture.host, kStream, -1.0F),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(
        table().audio_stream_seek(&fixture.host, kStream, std::numeric_limits<double>::infinity()),
        CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().audio_stream_set_volume(&fixture.host, kStream, -0.1F, 0.0F),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(fixture.audio.calls, 0);
}

CY_TEST_CASE("audio streams: stop, seek, volume and state reach the backend") {
    StreamHost<FakeStreams> fixture;
    CY_CHECK_EQ(table().audio_stream_stop(&fixture.host, kStream, 1.5F), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.audio.stopped, kStream);
    CY_CHECK_EQ(fixture.audio.stop_fade, 1.5F);
    CY_CHECK_EQ(table().audio_stream_stop(&fixture.host, 0, 0.0F), CY_RESULT_OK);  // null: nothing
    CY_CHECK_EQ(table().audio_stream_seek(&fixture.host, kStream, 9.25), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.audio.sought, 9.25);
    CY_CHECK_EQ(table().audio_stream_seek(&fixture.host, 3, 1.0), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().audio_stream_set_volume(&fixture.host, kStream, 0.25F, 1.0F), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.audio.set_volume, 0.25F);

    CyAudioStreamState state{};
    state.struct_size = sizeof(CyAudioStreamState);
    CY_CHECK_EQ(table().audio_stream_state(&fixture.host, kStream, &state), CY_RESULT_OK);
    CY_CHECK_EQ(state.flags, CY_AUDIO_STREAM_PLAYING | CY_AUDIO_STREAM_LOOPING);
    CY_CHECK_EQ(state.position_seconds, 12.5);
    CY_CHECK_EQ(state.duration_seconds, 32.0);
    CY_CHECK_EQ(state.gain, 0.75F);
    CY_CHECK_EQ(state.underruns, 2U);
    CY_CHECK_EQ(state.struct_size, sizeof(CyAudioStreamState));

    // A shorter caller gets its prefix and is told how much was written.
    CyAudioStreamState older{};
    older.struct_size = static_cast<uint32_t>(offsetof(CyAudioStreamState, gain));
    older.gain = 9.0F;
    CY_CHECK_EQ(table().audio_stream_state(&fixture.host, kStream, &older), CY_RESULT_OK);
    CY_CHECK_EQ(older.duration_seconds, 32.0);
    CY_CHECK_EQ(older.gain, 9.0F);
    CY_CHECK_EQ(older.struct_size, offsetof(CyAudioStreamState, gain));
    CY_CHECK_EQ(table().audio_stream_state(&fixture.host, 5, &state), CY_RESULT_NOT_FOUND);
}

CY_TEST_CASE("audio streams: while resimulating, the writes succeed and do nothing") {
    StreamHost<FakeStreams> fixture;
    fixture.host.game.clock.phase = CY_PHASE_FIXED_UPDATE;
    fixture.host.game.clock.flags = CY_TIME_RESIMULATING;
    const CyAudioStreamPlay request = theme();
    CyAudioStream stream = 77;
    CY_CHECK_EQ(table().audio_stream_play(&fixture.host, &request, &stream), CY_RESULT_OK);
    CY_CHECK_EQ(stream, 0U);
    CY_CHECK_EQ(table().audio_stream_stop(&fixture.host, kStream, 0.0F), CY_RESULT_OK);
    CY_CHECK_EQ(table().audio_stream_seek(&fixture.host, kStream, 1.0), CY_RESULT_OK);
    CY_CHECK_EQ(table().audio_stream_set_volume(&fixture.host, kStream, 1.0F, 0.0F), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.audio.calls, 0);
    CyAudioTrack track = 0;
    CY_CHECK_EQ(table().audio_find_track(&fixture.host, "theme", &track), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.audio.calls, 1);
}

CY_TEST_CASE("audio streams: a backend that predates streams answers UNAVAILABLE, and none NULL") {
    StreamHost<CuesOnly> fixture;
    CyAudioTrack track = 0;
    CY_CHECK_EQ(table().audio_find_track(&fixture.host, "theme", &track), CY_RESULT_UNAVAILABLE);
    const CyAudioStreamPlay request = theme();
    CyAudioStream stream = 0;
    CY_CHECK_EQ(table().audio_stream_play(&fixture.host, &request, &stream), CY_RESULT_UNAVAILABLE);

    cy::abi::Host bare{cy::system_allocator(cy::MemoryDomain::Scripting)};
    CY_CHECK_EQ(table().audio_stream_stop(&bare, kStream, 0.0F), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().audio_stream_play(nullptr, &request, &stream), CY_RESULT_INVALID_ARGUMENT);
}
