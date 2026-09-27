// SPDX-License-Identifier: MIT
// ABI 1.3's `audio_*` thunks, against a fake `AudioBackend`. `add-swift-game-api`.
//
// OWNER: implementer C. What is proven here is the boundary — every argument check, the phase list,
// UNAVAILABLE with no backend, the `struct_size` handling of `CyAudioPlay`, and the resimulation
// drop. What the real mixer does with a voice is `integration.game_backend_audio`'s.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/audio.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstddef>
#include <cstring>
#include <limits>
#include <string>

namespace {

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

constexpr CyAudioCue kExplosion = 0x1'0000'0003ULL;
constexpr CyAudioBus kMusic = 0x1'0000'0001ULL;
constexpr CyAudioVoice kVoice = 0x2'0000'0007ULL;

/// Records every call and answers from a two-entry catalogue.
class FakeAudio final : public cy::abi::game::AudioBackend {
public:
    CyResult find_cue(const char* name, CyAudioCue& out_cue) noexcept override {
        ++calls;
        if (std::string(name) != "explosion") {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no such cue");
        }
        out_cue = kExplosion;
        return CY_RESULT_OK;
    }
    CyResult play(const CyAudioPlay& request, CyAudioVoice& out_voice) noexcept override {
        ++calls;
        last_play = request;
        if (request.cue != kExplosion) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no such cue");
        }
        out_voice = kVoice;
        return CY_RESULT_OK;
    }
    CyResult stop(CyAudioVoice voice, cy::f32 fade) noexcept override {
        ++calls;
        stopped = voice;
        stop_fade = fade;
        return CY_RESULT_OK;
    }
    bool playing(CyAudioVoice voice) const noexcept override { return voice == kVoice; }
    CyResult find_bus(const char* name, CyAudioBus& out_bus) noexcept override {
        ++calls;
        if (std::string(name) != "Music") {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no such bus");
        }
        out_bus = kMusic;
        return CY_RESULT_OK;
    }
    CyResult set_bus_volume(CyAudioBus bus, cy::f32 volume, cy::f32 fade) noexcept override {
        ++calls;
        if (bus != kMusic) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no such bus");
        }
        bus_volume = volume;
        bus_fade = fade;
        return CY_RESULT_OK;
    }

    int calls = 0;
    CyAudioPlay last_play{};
    CyAudioVoice stopped = 0;
    cy::f32 stop_fade = -1.0F;
    cy::f32 bus_volume = -1.0F;
    cy::f32 bus_fade = -1.0F;
};

struct AudioHost {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    FakeAudio audio;

    AudioHost() { host.game.audio = &audio; }
};

CyAudioPlay explosion() noexcept {
    CyAudioPlay request{};
    request.struct_size = sizeof(CyAudioPlay);
    request.cue = kExplosion;
    request.flags = CY_AUDIO_PLAY_SPATIAL;
    request.position[0] = 1.0F;
    request.position[1] = 2.0F;
    request.position[2] = 3.0F;
    request.volume = 0.5F;
    return request;
}

}  // namespace

CY_TEST_CASE("audio: a cue and a bus resolve by name, and an unknown name is NOT_FOUND") {
    AudioHost fixture;
    CyAudioCue cue = 0;
    CY_CHECK_EQ(table().audio_find_cue(&fixture.host, "explosion", &cue), CY_RESULT_OK);
    CY_CHECK_EQ(cue, kExplosion);
    CyAudioCue untouched = 99;
    CY_CHECK_EQ(table().audio_find_cue(&fixture.host, "nope", &untouched), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(untouched, 99U);

    CyAudioBus bus = 0;
    CY_CHECK_EQ(table().audio_find_bus(&fixture.host, "Music", &bus), CY_RESULT_OK);
    CY_CHECK_EQ(bus, kMusic);
    CY_CHECK_EQ(table().audio_find_bus(&fixture.host, "Nope", &bus), CY_RESULT_NOT_FOUND);

    CY_CHECK_EQ(table().audio_find_cue(&fixture.host, nullptr, &cue), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().audio_find_bus(&fixture.host, "Music", nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("audio: play hands the backend the whole request and returns its voice") {
    AudioHost fixture;
    const CyAudioPlay request = explosion();
    CyAudioVoice voice = 0;
    CY_REQUIRE_EQ(table().audio_play(&fixture.host, &request, &voice), CY_RESULT_OK);
    CY_CHECK_EQ(voice, kVoice);
    CY_CHECK_EQ(fixture.audio.last_play.position[2], 3.0F);
    CY_CHECK_EQ(fixture.audio.last_play.volume, 0.5F);
    CY_CHECK(table().audio_voice_playing(&fixture.host, voice));

    // Fire and forget: no output pointer is fine.
    CY_CHECK_EQ(table().audio_play(&fixture.host, &request, nullptr), CY_RESULT_OK);

    CyAudioPlay unknown = request;
    unknown.cue = 5;
    CyAudioVoice untouched = 42;
    CY_CHECK_EQ(table().audio_play(&fixture.host, &unknown, &untouched), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(untouched, 42U);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("audio: an older caller's short CyAudioPlay reaches the backend zero-filled") {
    AudioHost fixture;
    CyAudioPlay request = explosion();
    // A caller that knows the struct up to `bus`: position, volume, pitch and fade are unknown to
    // it and must reach the backend as zero ("as authored"), not as whatever followed in memory.
    request.struct_size = static_cast<uint32_t>(offsetof(CyAudioPlay, position));
    request.volume = 7.0F;
    CY_REQUIRE_EQ(table().audio_play(&fixture.host, &request, nullptr), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.audio.last_play.struct_size, sizeof(CyAudioPlay));
    CY_CHECK_EQ(fixture.audio.last_play.cue, kExplosion);
    CY_CHECK_EQ(fixture.audio.last_play.volume, 0.0F);
    CY_CHECK_EQ(fixture.audio.last_play.position[0], 0.0F);

    request.struct_size = 3;
    CY_CHECK_EQ(table().audio_play(&fixture.host, &request, nullptr), CY_RESULT_INVALID_ARGUMENT);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("audio: play refuses a null request and non-finite or negative values") {
    AudioHost fixture;
    CY_CHECK_EQ(table().audio_play(&fixture.host, nullptr, nullptr), CY_RESULT_INVALID_ARGUMENT);

    CyAudioPlay request = explosion();
    request.volume = -1.0F;
    CY_CHECK_EQ(table().audio_play(&fixture.host, &request, nullptr), CY_RESULT_INVALID_ARGUMENT);
    request = explosion();
    request.pitch = std::numeric_limits<float>::quiet_NaN();
    CY_CHECK_EQ(table().audio_play(&fixture.host, &request, nullptr), CY_RESULT_INVALID_ARGUMENT);
    request = explosion();
    request.position[1] = std::numeric_limits<float>::infinity();
    CY_CHECK_EQ(table().audio_play(&fixture.host, &request, nullptr), CY_RESULT_INVALID_ARGUMENT);
    request = explosion();
    request.fade_in_seconds = -0.5F;
    CY_CHECK_EQ(table().audio_play(&fixture.host, &request, nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(fixture.audio.calls, 0);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("audio: while resimulating, play, stop and set_bus_volume succeed and do nothing") {
    AudioHost fixture;
    fixture.host.game.clock.phase = CY_PHASE_FIXED_UPDATE;
    fixture.host.game.clock.flags = CY_TIME_RESIMULATING;

    const CyAudioPlay request = explosion();
    CyAudioVoice voice = 77;
    CY_CHECK_EQ(table().audio_play(&fixture.host, &request, &voice), CY_RESULT_OK);
    CY_CHECK_EQ(voice, 0U);  // a null voice, written
    CY_CHECK_EQ(table().audio_stop(&fixture.host, kVoice, 0.1F), CY_RESULT_OK);
    CY_CHECK_EQ(table().audio_set_bus_volume(&fixture.host, kMusic, 0.2F, 0.0F), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.audio.calls, 0);

    // The reads still answer: they change nothing.
    CyAudioCue cue = 0;
    CY_CHECK_EQ(table().audio_find_cue(&fixture.host, "explosion", &cue), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.audio.calls, 1);
}

CY_TEST_CASE("audio: stop is idempotent and passes the fade; a null voice never reaches it") {
    AudioHost fixture;
    CY_CHECK_EQ(table().audio_stop(&fixture.host, kVoice, 0.25F), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.audio.stopped, kVoice);
    CY_CHECK_EQ(fixture.audio.stop_fade, 0.25F);

    fixture.audio.calls = 0;
    CY_CHECK_EQ(table().audio_stop(&fixture.host, 0, 0.0F), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.audio.calls, 0);
    CY_CHECK_EQ(table().audio_stop(&fixture.host, kVoice, -1.0F), CY_RESULT_INVALID_ARGUMENT);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("audio: a bus volume below zero is OUT_OF_RANGE and a NaN is INVALID_ARGUMENT") {
    AudioHost fixture;
    CY_CHECK_EQ(table().audio_set_bus_volume(&fixture.host, kMusic, 0.3F, 1.5F), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.audio.bus_volume, 0.3F);
    CY_CHECK_EQ(fixture.audio.bus_fade, 1.5F);

    CY_CHECK_EQ(table().audio_set_bus_volume(&fixture.host, kMusic, -0.1F, 0.0F),
                CY_RESULT_OUT_OF_RANGE);
    CY_CHECK_EQ(table().audio_set_bus_volume(&fixture.host, kMusic,
                                             std::numeric_limits<float>::quiet_NaN(), 0.0F),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().audio_set_bus_volume(&fixture.host, 9, 1.0F, 0.0F), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(fixture.audio.bus_volume, 0.3F);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("audio: every entry answers in every phase") {
    AudioHost fixture;
    const CyAudioPlay request = explosion();
    for (const CyPhase phase : {CY_PHASE_NONE, CY_PHASE_FIXED_UPDATE, CY_PHASE_FRAME_UPDATE}) {
        fixture.host.game.clock.phase = phase;
        CyAudioCue cue = 0;
        CY_CHECK_EQ(table().audio_find_cue(&fixture.host, "explosion", &cue), CY_RESULT_OK);
        CY_CHECK_EQ(table().audio_play(&fixture.host, &request, nullptr), CY_RESULT_OK);
        CY_CHECK_EQ(table().audio_stop(&fixture.host, kVoice, 0.0F), CY_RESULT_OK);
        CY_CHECK(table().audio_voice_playing(&fixture.host, kVoice));
    }
}

CY_TEST_CASE("audio: with no backend every entry is UNAVAILABLE, and playing is false") {
    cy::abi::Host host(cy::system_allocator(cy::MemoryDomain::Scripting));
    const CyAudioPlay request = explosion();
    CyAudioCue cue = 0;
    CyAudioBus bus = 0;
    CY_CHECK_EQ(table().audio_find_cue(&host, "explosion", &cue), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().audio_play(&host, &request, nullptr), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().audio_stop(&host, kVoice, 0.0F), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().audio_find_bus(&host, "Music", &bus), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().audio_set_bus_volume(&host, kMusic, 1.0F, 0.0F), CY_RESULT_UNAVAILABLE);
    CY_CHECK_FALSE(table().audio_voice_playing(&host, kVoice));
    CY_CHECK_FALSE(table().audio_voice_playing(nullptr, kVoice));
    CY_CHECK_EQ(table().audio_play(nullptr, &request, nullptr), CY_RESULT_INVALID_ARGUMENT);
    cy::abi::clear_last_error();
}
