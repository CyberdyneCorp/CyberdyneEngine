// SPDX-License-Identifier: MIT
// The audio server's streams, through the null backend: priming, underrun, crossfade curves,
// flushes and natural ends. Issue #87.
//
// The ring is filled by the case itself, so every frame the mixer reads is one the case chose and
// every underrun is one the case caused.

#include <cy/core/math/scalar.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/servers/audio/server.h>
#include <cy/test/test.h>

#include <cmath>
#include <vector>

using namespace cy;
using namespace cy::audio;

namespace {

[[nodiscard]] Allocator& heap() noexcept {
    return system_allocator(MemoryDomain::Audio);
}

constexpr u32 kRate = 48000;
constexpr u32 kBlock = 120;
/// Samples per frame: a usize, so an index computed from a frame count is computed in usize.
constexpr usize kStereo = 2;

struct Mixer {
    NullAudioBackend backend;
    AudioServer server{heap()};
    std::vector<f32> output;

    Mixer() {
        AudioBackendConfig backend_config;
        backend_config.requested.sample_rate = kRate;
        backend_config.requested.layout = ChannelLayout::Stereo;
        backend_config.requested.buffer_frames = kBlock;
        CY_REQUIRE(backend.initialize(backend_config).has_value());
        AudioServerConfig config;
        config.requested = backend_config.requested;
        config.block_frames = kBlock;
        CY_REQUIRE(server.configure(config).has_value());
        CY_REQUIRE(server.initialize_with(backend).has_value());
    }

    /// Mix `frames` (a multiple of the block) and keep them.
    void mix(u32 frames) {
        output.assign(static_cast<usize>(frames) * 2, 0.0F);
        CY_REQUIRE_EQ(backend.advance(output.data(), frames), frames);
        server.update(0.0F);
    }
};

/// Write `frames` frames of a constant stereo pair.
void feed(StreamRing& ring, u32 frames, f32 left, f32 right) {
    std::vector<f32> block(static_cast<usize>(frames) * 2);
    for (u32 frame = 0; frame < frames; ++frame) {
        block[frame * kStereo] = left;
        block[(frame * kStereo) + 1] = right;
    }
    CY_REQUIRE_EQ(ring.write(block.data(), frames), frames);
}

}  // namespace

CY_TEST_CASE("streams: a stream sounds once primed, and waiting to prime is not an underrun") {
    Mixer mixer;
    StreamRing ring(heap());
    CY_REQUIRE(ring.allocate(4800, 2).has_value());
    StreamDescription description;
    description.ring = &ring;
    description.prime_frames = 2400;
    const Expected<StreamHandle, Error> stream = mixer.server.create_stream(description);
    CY_REQUIRE(stream.has_value());
    CY_REQUIRE(mixer.server.start_stream(*stream, 0.0F).has_value());

    feed(ring, 1200, 0.5F, 0.5F);  // half of what priming needs
    mixer.mix(kBlock * 4);
    CY_CHECK_EQ(mixer.output[0], 0.0F);
    CY_CHECK(mixer.server.stream_status(*stream).state == StreamState::Priming);
    CY_CHECK_EQ(mixer.server.stream_status(*stream).underruns, 0U);

    feed(ring, 1200, 0.5F, 0.5F);
    mixer.mix(kBlock);
    CY_CHECK_EQ(mixer.output[0], 0.5F);
    CY_CHECK(mixer.server.stream_status(*stream).state == StreamState::Playing);
    CY_CHECK_EQ(mixer.server.stream_status(*stream).frames_mixed, kBlock);
}

CY_TEST_CASE("streams: a ring that runs dry plays silence and counts the underrun") {
    Mixer mixer;
    StreamRing ring(heap());
    CY_REQUIRE(ring.allocate(4800, 2).has_value());
    StreamDescription description;
    description.ring = &ring;
    const Expected<StreamHandle, Error> stream = mixer.server.create_stream(description);
    CY_REQUIRE(stream.has_value());
    CY_REQUIRE(mixer.server.start_stream(*stream, 0.0F).has_value());

    feed(ring, kBlock + 40, 0.25F, 0.25F);
    mixer.mix(kBlock * 3);  // one full block, forty frames, then nothing
    CY_CHECK_EQ(mixer.output[(kBlock + 39) * kStereo], 0.25F);
    CY_CHECK_EQ(mixer.output[(kBlock + 40) * kStereo], 0.0F);
    CY_CHECK_EQ(mixer.output[((kBlock * 3) - 1) * kStereo], 0.0F);
    const StreamStatus status = mixer.server.stream_status(*stream);
    CY_CHECK_EQ(status.underruns, 2U);
    CY_CHECK_EQ(status.underrun_frames, (kBlock - 40) + kBlock);
    // The game thread turned the count into the server's statistics; the log line beside it is
    // the diagnostic.
    CY_CHECK_EQ(mixer.server.statistics().stream_underruns, 2U);
    CY_CHECK_EQ(mixer.server.statistics().stream_underrun_frames, (kBlock - 40) + kBlock);
    // And the stream recovers by itself when frames arrive again: an underrun is not a stall.
    feed(ring, kBlock, 0.25F, 0.25F);
    mixer.mix(kBlock);
    CY_CHECK_EQ(mixer.output[0], 0.25F);
}

CY_TEST_CASE("streams: an equal-power crossfade holds the summed power at one, frame by frame") {
    // Two streams, one on each side: the left channel IS the outgoing gain, the right the incoming.
    Mixer mixer;
    StreamRing out_ring(heap());
    StreamRing in_ring(heap());
    CY_REQUIRE(out_ring.allocate(48000, 2).has_value());
    CY_REQUIRE(in_ring.allocate(48000, 2).has_value());
    feed(out_ring, 24000, 1.0F, 0.0F);
    feed(in_ring, 24000, 0.0F, 1.0F);
    StreamDescription description;
    description.ring = &out_ring;
    const Expected<StreamHandle, Error> outgoing = mixer.server.create_stream(description);
    description.ring = &in_ring;
    const Expected<StreamHandle, Error> incoming = mixer.server.create_stream(description);
    CY_REQUIRE(outgoing.has_value());
    CY_REQUIRE(incoming.has_value());
    CY_REQUIRE(mixer.server.start_stream(*outgoing, 0.0F).has_value());
    mixer.mix(kBlock);

    constexpr f32 kSeconds = 0.25F;
    constexpr u32 kFrames = 12000;
    CY_REQUIRE(mixer.server.start_stream(*incoming, kSeconds, FadeCurve::EqualPower).has_value());
    CY_REQUIRE(mixer.server.fade_stream(*outgoing, 0.0F, kSeconds, FadeCurve::EqualPower, true)
                   .has_value());
    mixer.mix(kFrames + kBlock);
    u32 wrong = 0;
    for (u32 frame = 0; frame < kFrames && wrong == 0; ++frame) {
        const f32 t = static_cast<f32>(frame + 1) / static_cast<f32>(kFrames);
        const f32 left = mixer.output[frame * kStereo];
        const f32 right = mixer.output[(frame * kStereo) + 1];
        const bool on_curve = std::fabs(left - std::cos(t * 0.5F * math::kPi)) < 1e-5F &&
                              std::fabs(right - std::sin(t * 0.5F * math::kPi)) < 1e-5F &&
                              std::fabs((left * left) + (right * right) - 1.0F) < 1e-5F;
        if (!on_curve) {
            wrong = frame + 1;
            CY_TEST_INFO("frame " << frame << ": outgoing " << left << ", incoming " << right);
        }
    }
    CY_CHECK_EQ(wrong, 0U);
    // The outgoing stream stopped itself at the end of its ramp, and the incoming one holds.
    CY_CHECK(mixer.server.stream_finished(*outgoing));
    CY_CHECK_EQ(mixer.output[((kFrames + 10) * kStereo) + 1], 1.0F);
}

CY_TEST_CASE("streams: a flush fades out across one block, drops the ring and primes again") {
    Mixer mixer;
    StreamRing ring(heap());
    CY_REQUIRE(ring.allocate(4800, 2).has_value());
    StreamDescription description;
    description.ring = &ring;
    description.prime_frames = 240;
    const Expected<StreamHandle, Error> stream = mixer.server.create_stream(description);
    CY_REQUIRE(stream.has_value());
    CY_REQUIRE(mixer.server.start_stream(*stream, 0.0F).has_value());
    feed(ring, 2400, 1.0F, 1.0F);
    mixer.mix(kBlock);

    CY_REQUIRE(mixer.server.flush_stream(*stream).has_value());
    mixer.mix(kBlock * 2);
    // One block ramping to silence, then silence while it primes — and no underrun for either.
    CY_CHECK_NEAR(mixer.output[0], 1.0F - (1.0F / kBlock), 1e-6F);
    CY_CHECK_EQ(mixer.output[(kBlock - 1) * kStereo], 0.0F);
    CY_CHECK_EQ(mixer.output[kBlock * kStereo], 0.0F);
    StreamStatus status = mixer.server.stream_status(*stream);
    CY_CHECK_EQ(status.flushes, 1U);
    CY_CHECK_EQ(ring.readable_frames(), 0U);
    CY_CHECK_EQ(status.underruns, 0U);

    // The new position ramps in over its first block rather than starting mid-waveform.
    feed(ring, 480, 1.0F, 1.0F);
    mixer.mix(kBlock * 2);
    CY_CHECK_NEAR(mixer.output[0], 1.0F / kBlock, 1e-6F);
    CY_CHECK_EQ(mixer.output[kBlock * kStereo], 1.0F);
}

CY_TEST_CASE("streams: a track that ends stops by itself, which is not an underrun") {
    Mixer mixer;
    StreamRing ring(heap());
    CY_REQUIRE(ring.allocate(4800, 2).has_value());
    StreamDescription description;
    description.ring = &ring;
    const Expected<StreamHandle, Error> stream = mixer.server.create_stream(description);
    CY_REQUIRE(stream.has_value());
    CY_REQUIRE(mixer.server.start_stream(*stream, 0.0F).has_value());
    feed(ring, 200, 0.5F, 0.5F);
    ring.mark_end();
    mixer.mix(kBlock * 3);
    CY_CHECK(mixer.server.stream_finished(*stream));
    CY_CHECK_EQ(mixer.server.stream_status(*stream).underruns, 0U);
    CY_CHECK_EQ(mixer.output[199 * kStereo], 0.5F);
    CY_CHECK_EQ(mixer.output[200 * kStereo], 0.0F);
    // Finished, so the slot is released at once and can be taken again.
    mixer.server.destroy_stream(*stream);
    CY_CHECK(mixer.server.create_stream(description).has_value());
}
