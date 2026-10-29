// SPDX-License-Identifier: MIT
// integration.audio_codecs: the miniaudio backend's decoders against the reference signal, their
// seeks against their own decode from the start, their memory and their cost. Issue #87. The
// ten-minute streamed Vorbis track is smoke.audio_stream_long (test_stream_long.cpp): ten minutes
// of decoding is seconds of CPU, which is past this suite's budget by design.
//
// The fixtures are content/audio/fixtures/tone.{flac,ogg,mp3}, made by tools/content/make_audio.py
// from cy/test/audio_signal.h's integer signal. Nothing here compares against a decode by the
// library under test unless the case says so — a seek is compared with a decode from the start,
// which is what "sample-accurate" means.

#include <cy/backends/audio/miniaudio_codecs.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/memory/tracking_allocator.h>
#include <cy/servers/audio/decoder.h>
#include <cy/test/audio_signal.h>
#include <cy/test/test.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace cy;
using namespace cy::audio;
namespace signal = cy::test::audio_signal;

namespace {

[[nodiscard]] Allocator& heap() noexcept {
    return system_allocator(MemoryDomain::Audio);
}

[[nodiscard]] std::vector<u8> fixture(const char* name) {
    const std::string path = std::string(CY_AUDIO_CONTENT_DIR) + "/fixtures/" + name;
    std::vector<u8> bytes = signal::read_file(path);
    CY_REQUIRE_MESSAGE(!bytes.empty(), "missing fixture " << path);
    return bytes;
}

/// A whole source, decoded once.
struct Decoded {
    explicit Decoded(const std::vector<u8>& bytes, Allocator& allocator = heap())
        : source(Span<const u8>(bytes.data(), bytes.size())), samples(heap()) {
        Expected<OpenedSource, Error> opened =
            miniaudio_audio_codecs().open_source(source, allocator);
        CY_REQUIRE_MESSAGE(opened.has_value(), (opened ? "" : opened.error().message));
        container = opened->format;
        format = opened->decoder->format();
        loop = opened->decoder->source_loop();
        resident = opened->decoder->resident_bytes();
        CY_REQUIRE(decode_all(*opened->decoder, samples).has_value());
    }
    [[nodiscard]] u64 frames() const { return samples.size() / format.channels; }

    MemoryByteSource source;
    Array<f32> samples;
    SourceFormat container = SourceFormat::Unknown;
    DecodedFormat format;
    SourceLoop loop;
    usize resident = 0;
};

/// Signal-to-noise of a decode against the reference, both channels, from `first` on.
[[nodiscard]] f64 snr_db(const Decoded& decoded, u64 first = 0) {
    f64 signal_power = 0.0;
    f64 noise_power = 0.0;
    for (u64 frame = first; frame < decoded.frames(); ++frame) {
        for (u32 channel = 0; channel < 2; ++channel) {
            const auto reference = static_cast<f64>(signal::reference_float(frame, channel));
            const f64 error = static_cast<f64>(decoded.samples[(frame * 2) + channel]) - reference;
            signal_power += reference * reference;
            noise_power += error * error;
        }
    }
    return 10.0 * std::log10(signal_power / std::max(noise_power, 1e-30));
}

/// Open a fresh decoder over a source file.
[[nodiscard]] DecoderPtr open(MemoryByteSource& source) {
    Expected<OpenedSource, Error> opened = miniaudio_audio_codecs().open_source(source, heap());
    CY_REQUIRE(opened.has_value());
    return static_cast<DecoderPtr&&>(opened->decoder);
}

/// Seek `decoder` to `frame` and require the next `count` frames to equal the decode from the
/// start.
void require_seek_exact(AudioDecoder& decoder, const Decoded& whole, u64 frame, u32 count) {
    CY_REQUIRE_MESSAGE(decoder.seek(frame).has_value(), "seek to " << frame);
    std::vector<f32> got(static_cast<usize>(count) * 2);
    const Expected<u32, Error> read = decoder.decode(got.data(), count);
    CY_REQUIRE(read.has_value());
    const u32 expected = static_cast<u32>(std::min<u64>(count, whole.frames() - frame));
    CY_REQUIRE_EQ(*read, expected);
    // The first difference is the whole story; a REQUIRE in a loop does not stop the case here.
    for (u32 index = 0; index < expected * 2; ++index) {
        if (got[index] != whole.samples[(frame * 2) + index]) {
            CY_CHECK_MESSAGE(got[index] == whole.samples[(frame * 2) + index],
                             "after a seek to " << frame << ", sample " << index << " differs");
            break;
        }
    }
}

/// The first frame whose samples differ from the reference signal's, or `kNone`.
constexpr u64 kNone = ~0ULL;
[[nodiscard]] u64 first_difference(const Decoded& decoded, u32 channels) {
    for (u64 frame = 0; frame < decoded.frames(); ++frame) {
        for (u32 channel = 0; channel < channels; ++channel) {
            if (decoded.samples[(frame * decoded.format.channels) + channel] !=
                signal::reference_float(frame, channel)) {
                return frame;
            }
        }
    }
    return kNone;
}

/// Positions a seek test visits: the edges, and a fixed pseudo-random spread, in a jumbled order so
/// forward and backward seeks alternate.
[[nodiscard]] std::vector<u64> seek_positions(u64 frames) {
    std::vector<u64> positions = {0, 1, 4095, 4096, 4097, frames / 2, frames - 3000, 44100};
    u64 state = 0x2545F4914F6CDD1DULL;
    for (u32 index = 0; index < 32; ++index) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        positions.push_back(state % (frames - 3000));
    }
    return positions;
}

}  // namespace

// --- Decoding against the reference --------------------------------------------------------------

CY_TEST_CASE("flac: the fixture decodes to the reference signal exactly, with its loop comments") {
    const std::vector<u8> bytes = fixture("tone.flac");
    const Decoded decoded(bytes);
    CY_CHECK(decoded.container == SourceFormat::Flac);
    CY_CHECK_EQ(decoded.format.sample_rate, signal::kFixtureRate);
    CY_CHECK_EQ(decoded.format.frame_count, signal::kShortFrames);
    CY_REQUIRE_EQ(decoded.frames(), signal::kShortFrames);
    CY_CHECK_EQ(first_difference(decoded, 2), kNone);
    CY_CHECK(decoded.loop.present);
    CY_CHECK_EQ(decoded.loop.start, 4410U);
    CY_CHECK_EQ(decoded.loop.end, 4410U + 22050U);
}

CY_TEST_CASE("wav: a 16-bit WAV decodes exactly, and its smpl loop is read as an exclusive end") {
    const std::vector<u8> bytes = signal::wav_bytes(signal::reference_samples(11025), 2,
                                                    signal::kFixtureRate, true, 100, 2099);
    const Decoded decoded(bytes);
    CY_CHECK(decoded.container == SourceFormat::Wav);
    CY_REQUIRE_EQ(decoded.frames(), 11025U);
    CY_CHECK_EQ(first_difference(decoded, 2), kNone);
    CY_CHECK(decoded.loop.present);
    CY_CHECK_EQ(decoded.loop.start, 100U);
    CY_CHECK_EQ(decoded.loop.end, 2100U);
}

CY_TEST_CASE("vorbis: the fixture decodes frame-aligned and close to the reference") {
    const std::vector<u8> bytes = fixture("tone.ogg");
    const Decoded decoded(bytes);
    CY_CHECK(decoded.container == SourceFormat::OggVorbis);
    CY_CHECK_EQ(decoded.format.frame_count, signal::kLongFrames);
    CY_CHECK_EQ(decoded.frames(), signal::kLongFrames);
    const f64 snr = snr_db(decoded);
    CY_TEST_MESSAGE("Vorbis q5 fixture: " << snr << " dB against the reference");
    CY_CHECK_GT(snr, 25.0);
    CY_CHECK(decoded.loop.present);
    CY_CHECK_EQ(decoded.loop.start, 44100U);
    CY_CHECK_EQ(decoded.loop.end, 44100U + 176400U);
}

CY_TEST_CASE("mp3: the LAME delay and padding are removed, so every frame is where it belongs") {
    const std::vector<u8> bytes = fixture("tone.mp3");
    const Decoded decoded(bytes);
    CY_CHECK(decoded.container == SourceFormat::Mp3);
    CY_CHECK_EQ(decoded.format.frame_count, signal::kLongFrames);
    CY_CHECK_EQ(decoded.frames(), signal::kLongFrames);
    const f64 snr = snr_db(decoded);
    CY_TEST_MESSAGE("MP3 192 kbit/s fixture: " << snr << " dB against the reference");
    CY_CHECK_GT(snr, 25.0);
}

// --- Seeking -------------------------------------------------------------------------------------

CY_TEST_CASE("vorbis: a seek anywhere lands on the samples a decode from the start delivers") {
    const std::vector<u8> bytes = fixture("tone.ogg");
    const Decoded whole(bytes);
    MemoryByteSource source(Span<const u8>(bytes.data(), bytes.size()));
    DecoderPtr decoder = open(source);
    for (const u64 frame : seek_positions(whole.frames())) {
        require_seek_exact(*decoder, whole, frame, 2048);
    }
    // The last frames, which the final page's granule position trims.
    require_seek_exact(*decoder, whole, whole.frames() - 700, 2048);
}

CY_TEST_CASE("flac: a seek anywhere is exact") {
    const std::vector<u8> bytes = fixture("tone.flac");
    const Decoded whole(bytes);
    MemoryByteSource source(Span<const u8>(bytes.data(), bytes.size()));
    DecoderPtr decoder = open(source);
    for (const u64 frame : seek_positions(whole.frames())) {
        require_seek_exact(*decoder, whole, frame, 1024);
    }
}

CY_TEST_CASE("mp3: a forward seek lands on its frame, where dr_mp3's own lands one delay early") {
    // The regression case for the defect codecs.cpp works around: after decoding from the start,
    // dr_mp3's forward seek subtracts a position that counts the LAME delay from a target that does
    // not, and lands 1105 frames early on this fixture. Forward and backward seeks, interleaved.
    const std::vector<u8> bytes = fixture("tone.mp3");
    const Decoded whole(bytes);
    MemoryByteSource source(Span<const u8>(bytes.data(), bytes.size()));
    DecoderPtr decoder = open(source);
    std::vector<f32> scratch(usize{1000} * 2);
    CY_REQUIRE(decoder->decode(scratch.data(), 1000).has_value());
    for (const u64 frame : {121515ULL, 139335ULL, 2377ULL, 45886ULL, 227383ULL, 228000ULL}) {
        require_seek_exact(*decoder, whole, frame, 1152);
    }
}

// --- Memory and cost -----------------------------------------------------------------------------

CY_TEST_CASE("decoders: every byte a decoder holds is counted, and none of it grows with use") {
    for (const char* name : {"tone.ogg", "tone.flac", "tone.mp3"}) {
        const std::vector<u8> bytes = fixture(name);
        TrackingAllocator tracker(heap(), MemoryDomain::Audio, "decoder");
        MemoryByteSource source(Span<const u8>(bytes.data(), bytes.size()));
        Expected<OpenedSource, Error> opened =
            miniaudio_audio_codecs().open_source(source, tracker);
        CY_REQUIRE(opened.has_value());
        const u64 after_open = tracker.live_bytes();
        std::vector<f32> block(usize{4096} * 2);
        for (;;) {
            const Expected<u32, Error> got = opened->decoder->decode(block.data(), 4096);
            CY_REQUIRE(got.has_value());
            if (*got == 0) {
                break;
            }
        }
        CY_REQUIRE(opened->decoder->seek(1000).has_value());
        const usize resident = opened->decoder->resident_bytes();
        CY_TEST_MESSAGE(std::string(name) << ": " << resident << " bytes resident, peak "
                                          << tracker.peak_bytes() << " allocated");
        CY_CHECK_EQ(tracker.live_bytes(), after_open);  // decoding allocated nothing more
        CY_CHECK_LE(tracker.live_bytes(), resident);    // and what it holds, it reports
        // A stereo q5 Vorbis stream's codebooks are most of its 210 KiB; FLAC and MP3 hold under
        // 100.
        CY_CHECK_LE(resident, 256U * 1024U);
        opened->decoder.reset();
        CY_CHECK_EQ(tracker.live_bytes(), 0U);
    }
}

CY_TEST_CASE("decoders: the decode cost per stream, measured") {
    // Reported, not asserted against a time — a shared CI runner is not an instrument. What is
    // asserted is that a stream costs a small fraction of a core, which no runner fails honestly.
    for (const char* name : {"tone.ogg", "tone.flac", "tone.mp3"}) {
        const std::vector<u8> bytes = fixture(name);
        MemoryByteSource source(Span<const u8>(bytes.data(), bytes.size()));
        DecoderPtr decoder = open(source);
        std::vector<f32> block(usize{4096} * 2);
        u64 frames = 0;
        const auto start = std::chrono::steady_clock::now();
        for (u32 pass = 0; pass < 3; ++pass) {
            CY_REQUIRE(decoder->seek(0).has_value());
            for (Expected<u32, Error> got = decoder->decode(block.data(), 4096); got && *got > 0;
                 got = decoder->decode(block.data(), 4096)) {
                frames += *got;
            }
        }
        const f64 seconds =
            std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count();
        const f64 audio_seconds = static_cast<f64>(frames) / signal::kFixtureRate;
        const f64 core_fraction = seconds / audio_seconds;
        CY_TEST_MESSAGE(std::string(name)
                        << ": " << (core_fraction * 100.0) << " % of one core per stream ("
                        << (audio_seconds / seconds) << "x real time)");
        CY_CHECK_LT(core_fraction, 0.25);
    }
}
