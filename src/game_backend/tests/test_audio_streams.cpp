// SPDX-License-Identifier: MIT
// `integration.game_backend_audio_streams`: ABI 1.18's streamed-track entries through the interface
// table, over a real `AudioServer`, `StreamPlayer` and `AudioAdapter` bound on a host. Issue #87.
//
// Every case runs over the server's null backend and refills inline, so what is mixed is a fact the
// case controls. Tracks are cooked PCM of a constant level on one side, so the mix says which track
// is sounding and at what gain.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/host.h>
#include <cy/audio/streaming.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/audio_backend.h>
#include <cy/servers/audio/server.h>
#include <cy/test/test.h>

#include <utility>
#include <vector>

namespace {

using namespace cy;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Audio);
}

constexpr u32 kRate = 48000;
constexpr u32 kBlock = 120;

/// A server over the null backend with the adapter bound on a host, and no scene.
struct AudioWorld {
    audio::NullAudioBackend device;
    audio::AudioServer server{allocator()};
    Array<f32> output{allocator()};
    abi::Host host{system_allocator(MemoryDomain::Scripting)};
    game_backend::AudioAdapter adapter{server, allocator()};

    AudioWorld() {
        audio::AudioBackendConfig device_config;
        device_config.requested.sample_rate = kRate;
        device_config.requested.layout = audio::ChannelLayout::Stereo;
        device_config.requested.buffer_frames = kBlock;
        CY_REQUIRE(device.initialize(device_config).has_value());
        audio::AudioServerConfig config;
        config.requested = device_config.requested;
        config.block_frames = kBlock;
        CY_REQUIRE(server.configure(config).has_value());
        CY_REQUIRE(server.initialize_with(device).has_value());
        CY_REQUIRE(output.resize(static_cast<usize>(kRate) * 2).has_value());
        game_backend::bind(host, &adapter);
    }
};

}  // namespace

namespace {

/// A cooked PCM track of a constant level on one side, so the mix says which track is sounding.
struct ConstantTrack {
    std::vector<u8> bytes;
    audio::MemoryByteSource source;

    ConstantTrack(u64 frames, bool left) {
        std::vector<u8> payload;
        payload.reserve(frames * 4);
        for (u64 frame = 0; frame < frames; ++frame) {
            for (const i16 sample : {left ? i16{16384} : i16{0}, left ? i16{0} : i16{16384}}) {
                const auto bits = static_cast<u16>(sample);
                payload.push_back(static_cast<u8>(bits & 0xFFU));
                payload.push_back(static_cast<u8>(bits >> 8U));
            }
        }
        audio::CookedAudioHeader header;
        header.codec = audio::AudioCodec::Pcm16;
        header.load_mode = audio::AudioLoadMode::Compressed;
        header.channels = 2;
        header.sample_rate = kRate;
        header.frame_count = frames;
        header.looping = true;
        Array<u8> cooked(allocator());
        CY_REQUIRE(audio::write_cooked_audio(header, Span<const u8>(payload.data(), payload.size()),
                                             cooked)
                       .has_value());
        bytes.assign(cooked.begin(), cooked.end());
        source.reset(Span<const u8>(bytes.data(), bytes.size()));
    }
};

/// The audio world plus a stream player on the adapter, refilling inline.
struct StreamWorld {
    AudioWorld audio;
    audio::InlineRefillExecutor executor;
    audio::StreamPlayer player{audio.server, executor, allocator()};
    ConstantTrack left{u64{kRate} * 4, true};
    ConstantTrack right{u64{kRate} * 4, false};

    StreamWorld() {
        CY_REQUIRE(player.configure({}).has_value());
        audio.adapter.bind_streams(&player);
        for (const auto& [name, track] : {std::pair{"left", &left}, std::pair{"right", &right}}) {
            const auto cooked = audio::track_from_cooked(track->source);
            CY_REQUIRE(cooked.has_value());
            CY_REQUIRE(audio.adapter.add_track(Name::intern(name), *cooked).has_value());
        }
    }

    /// One game frame: the adapter's update (which pumps the player), then 10 ms of mixing.
    void frame() {
        audio.adapter.update(0.01F);
        CY_REQUIRE_EQ(audio.device.advance(audio.output.data(), 480), 480U);
        audio.server.update(0.01F);
    }

    [[nodiscard]] CyAudioStream play(const char* name, CyAudioStream from = 0, float fade = 0.0F) {
        CyAudioTrack track = 0;
        CY_REQUIRE_EQ(table().audio_find_track(&audio.host, name, &track), CY_RESULT_OK);
        CyAudioStreamPlay request{};
        request.struct_size = sizeof(CyAudioStreamPlay);
        request.track = track;
        request.crossfade_from = from;
        request.fade_seconds = fade;
        CyAudioStream stream = 0;
        CY_REQUIRE_EQ(table().audio_stream_play(&audio.host, &request, &stream), CY_RESULT_OK);
        return stream;
    }
};

}  // namespace

CY_TEST_CASE("audio backend: a track streams, crossfades to another, seeks and reports itself") {
    StreamWorld fixture;
    const CyAudioStream left = fixture.play("left");
    CY_CHECK_NE(left, 0U);
    for (u32 frame = 0; frame < 10; ++frame) {
        fixture.frame();
    }
    CY_CHECK_EQ(fixture.audio.output[0], 0.5F);  // the left track, at full gain
    CY_CHECK_EQ(fixture.audio.output[1], 0.0F);

    // A one-second crossfade to the right track, then a little past it.
    const CyAudioStream right = fixture.play("right", left, 1.0F);
    for (u32 frame = 0; frame < 120; ++frame) {
        fixture.frame();
    }
    CY_CHECK_EQ(fixture.audio.output[0], 0.0F);
    CY_CHECK_EQ(fixture.audio.output[1], 0.5F);
    CyAudioStreamState state{};
    state.struct_size = sizeof(CyAudioStreamState);
    CY_CHECK_EQ(table().audio_stream_state(&fixture.audio.host, left, &state), CY_RESULT_NOT_FOUND);
    abi::clear_last_error();
    CY_REQUIRE_EQ(table().audio_stream_state(&fixture.audio.host, right, &state), CY_RESULT_OK);
    CY_CHECK_EQ(state.flags, CY_AUDIO_STREAM_PLAYING | CY_AUDIO_STREAM_LOOPING);
    CY_CHECK_EQ(state.duration_seconds, 4.0);
    CY_CHECK_EQ(state.gain, 1.0F);
    CY_CHECK_EQ(state.underruns, 0U);

    CY_REQUIRE_EQ(table().audio_stream_seek(&fixture.audio.host, right, 3.0), CY_RESULT_OK);
    for (u32 frame = 0; frame < 30; ++frame) {
        fixture.frame();
    }
    CY_REQUIRE_EQ(table().audio_stream_state(&fixture.audio.host, right, &state), CY_RESULT_OK);
    CY_CHECK_GE(state.position_seconds, 3.0);
    CY_CHECK_LT(state.position_seconds, 3.5);

    CY_REQUIRE_EQ(table().audio_stream_set_volume(&fixture.audio.host, right, 0.5F, 0.0F),
                  CY_RESULT_OK);
    fixture.frame();
    fixture.frame();
    CY_CHECK_EQ(fixture.audio.output[1], 0.25F);

    CY_REQUIRE_EQ(table().audio_stream_stop(&fixture.audio.host, right, 0.0F), CY_RESULT_OK);
    fixture.frame();
    fixture.frame();
    CY_CHECK_EQ(table().audio_stream_state(&fixture.audio.host, right, &state),
                CY_RESULT_NOT_FOUND);
    abi::clear_last_error();
    // Idempotent: the stream has ended and stopping it again is fine.
    CY_CHECK_EQ(table().audio_stream_stop(&fixture.audio.host, right, 0.0F), CY_RESULT_OK);
}

CY_TEST_CASE("audio backend: without a stream player the stream entries answer UNAVAILABLE") {
    AudioWorld fixture;
    CyAudioTrack track = 0;
    CY_CHECK_EQ(table().audio_find_track(&fixture.host, "left", &track), CY_RESULT_UNAVAILABLE);
    abi::clear_last_error();
}
