// SPDX-License-Identifier: MIT
// smoke.audio_stream_long: a streamed Vorbis track played for ten minutes through the stream
// player, the null backend and the miniaudio backend's decoder. Issue #87.
//
// `audio`: "**WHEN** a long music track plays **THEN** it SHALL be streamed and decoded ahead, with
// only the ring buffer resident." Ten minutes of 44.1 kHz Vorbis converted to 48 kHz is seconds of
// CPU, which is why this is smoke rather than integration. The track is the six-second fixture
// looping over its loop comments' range a hundred times — the decoder seeks back by page bisection
// every lap — and the allocator the player was given counts every byte it ever held.

#include <cy/audio/streaming.h>
#include <cy/backends/audio/miniaudio_codecs.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/memory/tracking_allocator.h>
#include <cy/servers/audio/server.h>
#include <cy/test/audio_signal.h>
#include <cy/test/test.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace cy;
using namespace cy::audio;
namespace sig = cy::test::audio_signal;

namespace {

[[nodiscard]] Allocator& heap() noexcept {
    return system_allocator(MemoryDomain::Audio);
}

[[nodiscard]] std::vector<u8> fixture(const char* name) {
    const std::string path = std::string(CY_AUDIO_CONTENT_DIR) + "/fixtures/" + name;
    std::vector<u8> bytes = sig::read_file(path);
    CY_REQUIRE_MESSAGE(!bytes.empty(), "missing fixture " << path);
    return bytes;
}

}  // namespace

CY_TEST_CASE(
    "stream player: ten minutes of a looping Vorbis track hold only the ring and decoder") {
    const std::vector<u8> bytes = fixture("tone.ogg");
    CookedAudioHeader header;
    header.codec = AudioCodec::Vorbis;
    header.load_mode = AudioLoadMode::Compressed;
    header.channels = 2;
    header.sample_rate = sig::kFixtureRate;
    header.frame_count = sig::kLongFrames;
    header.looping = true;
    header.loop_start = 44100;
    header.loop_end = 220500;
    Array<u8> cooked(heap());
    CY_REQUIRE(
        write_cooked_audio(header, Span<const u8>(bytes.data(), bytes.size()), cooked).has_value());
    MemoryByteSource asset(cooked.span());

    NullAudioBackend backend;
    AudioBackendConfig backend_config;
    backend_config.requested.sample_rate = 48000;
    backend_config.requested.buffer_frames = 960;
    CY_REQUIRE(backend.initialize(backend_config).has_value());
    AudioServer server(heap());
    AudioServerConfig config;
    config.requested = backend_config.requested;
    config.block_frames = 240;
    CY_REQUIRE(server.configure(config).has_value());
    CY_REQUIRE(server.initialize_with(backend).has_value());

    TrackingAllocator tracker(heap(), MemoryDomain::Audio, "stream");
    InlineRefillExecutor executor;
    StreamPlayer player(server, executor, tracker);
    StreamPlayerConfig player_config;
    player_config.capacity = 1;
    player_config.codecs = &miniaudio_audio_codecs();
    CY_REQUIRE(player.configure(player_config).has_value());
    const Expected<TrackSource, Error> track = track_from_cooked(asset);
    CY_REQUIRE(track.has_value());
    CY_REQUIRE(player.add_track(Name::intern("tone"), *track).has_value());
    const Expected<StreamId, Error> stream = player.play(Name::intern("tone"), {});
    CY_REQUIRE(stream.has_value());

    // Ten minutes at 48 kHz, a 20 ms device buffer at a time, pumped every buffer like a frame.
    std::vector<f32> output(usize{960} * 2);
    u64 peak_after_first_minute = 0;
    const u64 total_buffers = (10ULL * 60ULL * 48000ULL) / 960ULL;
    // The refills are timed apart from the mixing: inline, each pump's refill IS the decode job's
    // work — decoding, converting 44.1 to 48 kHz and writing the ring — so its time over ten
    // minutes of audio is a stream's decode cost as a fraction of one core.
    f64 refill_seconds = 0.0;
    for (u64 buffer = 0; buffer < total_buffers; ++buffer) {
        const auto before = std::chrono::steady_clock::now();
        player.pump();
        refill_seconds +=
            std::chrono::duration<f64>(std::chrono::steady_clock::now() - before).count();
        CY_REQUIRE_EQ(backend.advance(output.data(), 960), 960U);
        server.update(0.02F);
        if (buffer == total_buffers / 10) {
            peak_after_first_minute = tracker.peak_bytes();
        }
    }
    CY_TEST_MESSAGE("decode, resample and refill: " << refill_seconds << " s for 600 s of audio, "
                                                    << (refill_seconds / 6.0) << " % of one core");
    const StreamReport report = player.report(*stream);
    CY_CHECK(report.live);
    CY_CHECK_EQ(report.underruns, 0U);
    CY_CHECK_GT(report.refills, 1000U);
    // The bound: one ring, one decode side, and the player's own bookkeeping — nothing that grows
    // with the minutes played.
    CY_CHECK_EQ(tracker.peak_bytes(), peak_after_first_minute);
    CY_CHECK_LE(tracker.peak_bytes(), report.ring_bytes + report.decoder_bytes + 4096U);
    CY_TEST_MESSAGE("ten minutes of Vorbis: peak " << tracker.peak_bytes() << " bytes (ring "
                                                   << report.ring_bytes << ", decode side "
                                                   << report.decoder_bytes << ")");
    CY_CHECK(player.diagnostics().empty());
}
