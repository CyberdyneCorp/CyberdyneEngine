// SPDX-License-Identifier: MIT
// integration.audio_stream: the cooked format, the resampler, loudness, the stream ring, fade
// curves and the stream source over the built-in PCM decoder. Issue #87.
//
// No device, no codec library, no file: every byte a case decodes is one the case wrote.

#include <cy/core/memory/system_allocator.h>
#include <cy/servers/audio/cooked_audio.h>
#include <cy/servers/audio/decoder.h>
#include <cy/servers/audio/loudness.h>
#include <cy/servers/audio/resampler.h>
#include <cy/servers/audio/stream.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

using namespace cy;
using namespace cy::audio;

namespace {

[[nodiscard]] Allocator& heap() noexcept {
    return system_allocator(MemoryDomain::Audio);
}

/// A PCM cooked track whose every sample encodes its frame: left is frame / 32768 wrapped, right is
/// its negation. A frame read anywhere says where it came from.
[[nodiscard]] std::vector<u8> ramp_track(u64 frames, u32 rate, bool looping = false,
                                         u64 loop_start = 0, u64 loop_end = 0) {
    std::vector<u8> payload;
    payload.reserve(frames * 4);
    for (u64 frame = 0; frame < frames; ++frame) {
        const auto value = static_cast<i16>(static_cast<i32>(frame % 32768U) - 16384);
        const auto negated = static_cast<i16>(-value);
        for (const i16 sample : {value, negated}) {
            const auto bits = static_cast<u16>(sample);
            payload.push_back(static_cast<u8>(bits & 0xFFU));
            payload.push_back(static_cast<u8>(bits >> 8U));
        }
    }
    CookedAudioHeader header;
    header.codec = AudioCodec::Pcm16;
    header.load_mode = AudioLoadMode::Streamed;
    header.channels = 2;
    header.sample_rate = rate;
    header.frame_count = frames;
    header.looping = looping;
    header.loop_start = loop_start;
    header.loop_end = loop_end;
    Array<u8> out(heap());
    CY_REQUIRE(write_cooked_audio(header, Span<const u8>(payload.data(), payload.size()), out)
                   .has_value());
    return {out.begin(), out.end()};
}

[[nodiscard]] f32 ramp_value(u64 frame) noexcept {
    return static_cast<f32>(static_cast<i32>(frame % 32768U) - 16384) / 32768.0F;
}

/// Open a stream source over a cooked PCM track, converting to `rate` stereo.
struct PcmStream {
    explicit PcmStream(const std::vector<u8>& bytes, u32 rate = 48000, u32 chunk = 512)
        : asset(Span<const u8>(bytes.data(), bytes.size())), source(heap()), ring(heap()) {
        const Expected<CookedAudioView, Error> view =
            parse_cooked_audio(Span<const u8>(bytes.data(), bytes.size()));
        CY_REQUIRE(view.has_value());
        header = view->header;
        Expected<DecoderPtr, Error> decoder =
            open_cooked_audio(builtin_audio_codecs(), header, asset, payload, heap());
        CY_REQUIRE(decoder.has_value());
        CY_REQUIRE(source
                       .open(static_cast<DecoderPtr&&>(*decoder), stream_timing(header),
                             StreamFormat{rate, 2, chunk})
                       .has_value());
        CY_REQUIRE(ring.allocate(4096, 2).has_value());
    }

    /// Fill and drain `frames` frames through the ring.
    [[nodiscard]] std::vector<f32> pull(u64 frames) {
        std::vector<f32> out;
        std::vector<f32> block(usize{1024} * 2);
        while (out.size() < frames * 2) {
            CY_REQUIRE(source.fill(ring, 4).has_value());
            const u32 want = static_cast<u32>(std::min<u64>(1024, frames - (out.size() / 2)));
            const u32 got = ring.read(block.data(), want);
            out.insert(out.end(), block.begin(), block.begin() + (static_cast<long>(got) * 2));
            if (got == 0 && ring.drained()) {
                break;
            }
        }
        return out;
    }

    MemoryByteSource asset;
    RangeByteSource payload;
    CookedAudioHeader header;
    StreamSource source;
    StreamRing ring;
};

}  // namespace

// --- The cooked format ---------------------------------------------------------------------------

CY_TEST_CASE("cooked audio: every header field survives the bytes") {
    CookedAudioHeader header;
    header.codec = AudioCodec::Vorbis;
    header.load_mode = AudioLoadMode::Streamed;
    header.channels = 2;
    header.sample_rate = 44100;
    header.play_start = 1105;
    header.frame_count = 264600;
    header.looping = true;
    header.loop_start = 44100;
    header.loop_end = 220500;
    header.peak_dbfs = -6.25F;
    header.integrated_lufs = -18.5F;
    header.gain_db = 2.5F;
    const u8 payload[5] = {1, 2, 3, 4, 5};
    Array<u8> bytes(heap());
    CY_REQUIRE(write_cooked_audio(header, Span<const u8>(payload), bytes).has_value());
    CY_CHECK_EQ(bytes.size(), kCookedAudioHeaderBytes + 5);

    const Expected<CookedAudioView, Error> view = parse_cooked_audio(bytes.span());
    CY_REQUIRE(view.has_value());
    const CookedAudioHeader& read = view->header;
    CY_CHECK(read.codec == AudioCodec::Vorbis);
    CY_CHECK(read.load_mode == AudioLoadMode::Streamed);
    CY_CHECK_EQ(read.channels, 2);
    CY_CHECK_EQ(read.sample_rate, 44100U);
    CY_CHECK_EQ(read.play_start, 1105U);
    CY_CHECK_EQ(read.frame_count, 264600U);
    CY_CHECK(read.looping);
    CY_CHECK_EQ(read.loop_start, 44100U);
    CY_CHECK_EQ(read.loop_end, 220500U);
    CY_CHECK_EQ(read.peak_dbfs, -6.25F);
    CY_CHECK_EQ(read.integrated_lufs, -18.5F);
    CY_CHECK_EQ(read.gain_db, 2.5F);
    CY_CHECK_EQ(view->payload.size(), 5U);
    CY_CHECK_EQ(view->payload[4], 5);
}

CY_TEST_CASE("cooked audio: an inconsistent header is refused when it is written") {
    Array<u8> bytes(heap());
    const u8 payload[8] = {};
    CookedAudioHeader pcm;
    pcm.channels = 2;
    pcm.sample_rate = 48000;
    pcm.frame_count = 3;  // 12 bytes of stereo PCM, and there are 8
    CY_CHECK_FALSE(write_cooked_audio(pcm, Span<const u8>(payload), bytes).has_value());

    pcm.frame_count = 2;
    pcm.looping = true;
    pcm.loop_start = 2;  // a loop that starts where it ends
    CY_CHECK_FALSE(write_cooked_audio(pcm, Span<const u8>(payload), bytes).has_value());

    CookedAudioHeader decoded_vorbis;
    decoded_vorbis.codec = AudioCodec::Vorbis;
    decoded_vorbis.load_mode = AudioLoadMode::Decoded;
    decoded_vorbis.channels = 1;
    decoded_vorbis.sample_rate = 48000;
    decoded_vorbis.frame_count = 10;
    CY_CHECK_FALSE(write_cooked_audio(decoded_vorbis, Span<const u8>(payload), bytes).has_value());

    const u8 garbage[kCookedAudioHeaderBytes] = {'R', 'I', 'F', 'F'};
    CY_CHECK_FALSE(parse_cooked_audio(Span<const u8>(garbage)).has_value());
}

CY_TEST_CASE("source formats: each container is recognised by its first bytes") {
    const u8 wav[12] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    const u8 flac[4] = {'f', 'L', 'a', 'C'};
    const u8 id3[3] = {'I', 'D', '3'};
    const u8 mpeg[2] = {0xFF, 0xFB};
    u8 ogg[64] = {'O', 'g', 'g', 'S'};
    ogg[26] = 1;  // one segment
    ogg[28] = 1;  // identification packet
    const char vorbis[] = "vorbis";
    std::copy(vorbis, vorbis + 6, ogg + 29);
    CY_CHECK(sniff_source_format(Span<const u8>(wav)) == SourceFormat::Wav);
    CY_CHECK(sniff_source_format(Span<const u8>(flac)) == SourceFormat::Flac);
    CY_CHECK(sniff_source_format(Span<const u8>(id3)) == SourceFormat::Mp3);
    CY_CHECK(sniff_source_format(Span<const u8>(mpeg)) == SourceFormat::Mp3);
    CY_CHECK(sniff_source_format(Span<const u8>(ogg)) == SourceFormat::OggVorbis);
    const char opus[] = "OpusHead";
    std::copy(opus, opus + 8, ogg + 28);  // an Ogg of Opus is not a Vorbis stream
    CY_CHECK(sniff_source_format(Span<const u8>(ogg)) == SourceFormat::Unknown);
}

// --- The resampler -------------------------------------------------------------------------------

CY_TEST_CASE("resampler: equal rates pass through bit for bit") {
    Resampler resampler(heap());
    CY_REQUIRE(resampler.configure(48000, 48000, 2).has_value());
    CY_CHECK(resampler.passthrough());
    const f32 input[6] = {0.1F, -0.2F, 0.3F, -0.4F, 0.5F, -0.6F};
    f32 output[6] = {};
    CY_CHECK_EQ(resampler.process(input, 3, output), 3U);
    CY_CHECK(std::equal(input, input + 6, output));
}

CY_TEST_CASE("resampler: 44.1 to 48 kHz keeps a sine's frequency, level and timing") {
    // One second in owes exactly 48 000 frames out, and output frame n is the input at
    // n * 44100 / 48000 — so the converted sine is the same sine evaluated at the output rate.
    constexpr u32 kIn = 44100;
    constexpr u32 kOut = 48000;
    constexpr f64 kHz = 997.0;
    std::vector<f32> input(kIn);
    for (u32 frame = 0; frame < kIn; ++frame) {
        input[frame] = static_cast<f32>(0.5 * std::sin(2.0 * std::numbers::pi * kHz * frame / kIn));
    }
    Resampler resampler(heap());
    CY_REQUIRE(resampler.configure(kIn, kOut, 1).has_value());
    std::vector<f32> output(resampler.max_output(kIn) + resampler.max_output(64));
    u32 written = resampler.process(input.data(), kIn, output.data());
    written += resampler.drain(output.data() + written);
    CY_CHECK_EQ(written, kOut);

    f64 worst = 0.0;
    for (u32 frame = 64; frame + 64 < kOut; ++frame) {
        const f64 expected = 0.5 * std::sin(2.0 * std::numbers::pi * kHz * frame / kOut);
        worst = std::max(worst, std::fabs(static_cast<f64>(output[frame]) - expected));
    }
    CY_TEST_MESSAGE("largest error against the ideal 48 kHz sine: " << worst);
    CY_CHECK_LT(worst, 1e-4);  // -74 dB below a -6 dBFS tone, away from the edges
}

CY_TEST_CASE("resampler: a constant stays exactly constant, and chunking changes nothing") {
    Resampler whole(heap());
    Resampler chunked(heap());
    CY_REQUIRE(whole.configure(22050, 48000, 2).has_value());
    CY_REQUIRE(chunked.configure(22050, 48000, 2).has_value());
    std::vector<f32> input(usize{22050} * 2, 0.25F);
    std::vector<f32> one((static_cast<usize>(whole.max_output(22050)) * 2) + 1024);
    std::vector<f32> many(one.size());
    const u32 written = whole.process(input.data(), 22050, one.data());
    u32 pieces = 0;
    for (u32 at = 0; at < 22050; at += 1000) {
        const u32 count = std::min(1000U, 22050U - at);
        pieces += chunked.process(input.data() + (static_cast<usize>(at) * 2), count,
                                  many.data() + (static_cast<usize>(pieces) * 2));
    }
    CY_CHECK_EQ(written, pieces);
    CY_CHECK(std::equal(one.begin(), one.begin() + (static_cast<std::ptrdiff_t>(written) * 2),
                        many.begin()));
    for (u32 index = 64 * 2; index < written * 2; ++index) {
        CY_CHECK_NEAR(one[index], 0.25F, 1e-6F);
    }
}

// --- Loudness ------------------------------------------------------------------------------------

CY_TEST_CASE("loudness: a stereo 1 kHz sine at -23 dBFS reads -23 LUFS (EBU Tech 3341)") {
    constexpr u32 kRate = 48000;
    constexpr u32 kFrames = kRate * 20;
    const f64 amplitude = std::pow(10.0, -23.0 / 20.0);
    std::vector<f32> frames(static_cast<usize>(kFrames) * 2);
    for (u32 frame = 0; frame < kFrames; ++frame) {
        const auto sample =
            static_cast<f32>(amplitude * std::sin(2.0 * std::numbers::pi * 1000.0 * frame / kRate));
        frames[static_cast<usize>(frame) * 2] = sample;
        frames[(static_cast<usize>(frame) * 2) + 1] = sample;
    }
    const Expected<LoudnessMeasurement, Error> measured =
        measure_loudness(frames.data(), kFrames, 2, kRate, heap());
    CY_REQUIRE(measured.has_value());
    CY_CHECK_NEAR(measured->integrated_lufs, -23.0F, 0.1F);
    CY_CHECK_NEAR(measured->peak_dbfs, -23.0F, 0.01F);

    std::vector<f32> silence(static_cast<usize>(kRate) * 2 * 2, 0.0F);
    const Expected<LoudnessMeasurement, Error> quiet =
        measure_loudness(silence.data(), u64{kRate} * 2, 2, kRate, heap());
    CY_REQUIRE(quiet.has_value());
    CY_CHECK_LE(quiet->integrated_lufs, kAudioSilenceDb);
}

// --- The ring and the curves ---------------------------------------------------------------------

CY_TEST_CASE("stream ring: frames come out in order across the wrap, and a full ring refuses") {
    StreamRing ring(heap());
    CY_REQUIRE(ring.allocate(5, 1).has_value());
    const f32 first[4] = {1, 2, 3, 4};
    CY_CHECK_EQ(ring.write(first, 4), 4U);
    f32 out[8] = {};
    CY_CHECK_EQ(ring.read(out, 3), 3U);
    const f32 second[6] = {5, 6, 7, 8, 9, 10};
    CY_CHECK_EQ(ring.write(second, 6), 4U);  // one frame left from before, four of room
    CY_CHECK_EQ(ring.writable_frames(), 0U);
    CY_CHECK_EQ(ring.read(out, 8), 5U);
    const f32 expected[5] = {4, 5, 6, 7, 8};
    CY_CHECK(std::equal(expected, expected + 5, out));
    CY_CHECK_FALSE(ring.drained());
    ring.mark_end();
    CY_CHECK(ring.drained());
    CY_CHECK_EQ(ring.frames_read(), 8U);
}

CY_TEST_CASE("fade curves: equal power keeps the summed power at one, linear is a line") {
    for (u32 step = 0; step <= 100; ++step) {
        const f32 t = static_cast<f32>(step) / 100.0F;
        const f32 out = fade_curve_gain(FadeCurve::EqualPower, 1.0F, 0.0F, t);
        const f32 in = fade_curve_gain(FadeCurve::EqualPower, 0.0F, 1.0F, t);
        CY_CHECK_NEAR((out * out) + (in * in), 1.0F, 1e-6F);
        CY_CHECK_NEAR(fade_curve_gain(FadeCurve::Linear, 0.2F, 0.6F, t), 0.2F + (0.4F * t), 1e-6F);
    }
    CY_CHECK_EQ(fade_curve_gain(FadeCurve::EqualPower, 1.0F, 0.0F, 1.0F), 0.0F);
}

// --- The stream source ---------------------------------------------------------------------------

CY_TEST_CASE("stream source: a loop wraps sample-accurately, with no frame lost or repeated") {
    const std::vector<u8> track = ramp_track(1000, 48000, true, 300, 700);
    PcmStream stream(track);
    const std::vector<f32> out = stream.pull(2000);
    CY_REQUIRE_EQ(out.size(), 4000U);
    // 0..699, then 300..699 again and again.
    for (u64 index = 0; index < 2000; ++index) {
        const u64 source = (index < 700) ? index : 300 + ((index - 700) % 400);
        if (out[index * 2] != ramp_value(source) || out[(index * 2) + 1] != -ramp_value(source)) {
            CY_CHECK_MESSAGE(out[index * 2] == ramp_value(source),
                             "frame " << index << " should be source frame " << source);
            break;  // the first wrong frame says it; a REQUIRE would not stop the case here
        }
    }
}

CY_TEST_CASE("stream source: a track that ends marks the ring's end after its last frame") {
    const std::vector<u8> track = ramp_track(1500, 48000);
    PcmStream stream(track);
    const std::vector<f32> out = stream.pull(4000);
    CY_CHECK_EQ(out.size(), 3000U);
    CY_CHECK(stream.ring.drained());
    CY_CHECK(stream.source.ended());
    CY_CHECK_EQ(out[usize{1499} * 2], ramp_value(1499));
}

CY_TEST_CASE("stream source: a seek lands on the frame it names") {
    const std::vector<u8> track = ramp_track(5000, 48000);
    PcmStream stream(track);
    CY_REQUIRE(stream.source.seek(3210).has_value());
    stream.ring.reset();
    const std::vector<f32> out = stream.pull(10);
    CY_REQUIRE_EQ(out.size(), 20U);
    for (u64 index = 0; index < 10; ++index) {
        CY_CHECK_EQ(out[index * 2], ramp_value(3210 + index));
    }
    CY_CHECK_FALSE(stream.source.seek(5000).has_value());
}

CY_TEST_CASE("stream source: its memory does not grow with the track") {
    const std::vector<u8> short_track = ramp_track(48000, 44100);
    const std::vector<u8> long_track = ramp_track(u64{48000} * 30, 44100);
    PcmStream a(short_track);
    PcmStream b(long_track);
    (void)a.pull(20000);
    (void)b.pull(20000);
    CY_CHECK_EQ(a.source.resident_bytes(), b.source.resident_bytes());
    CY_TEST_MESSAGE("a 44.1 kHz PCM stream's decode side holds " << a.source.resident_bytes()
                                                                 << " bytes");
}
