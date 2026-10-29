// SPDX-License-Identifier: MIT
#ifndef CY_TEST_AUDIO_SIGNAL_H
#define CY_TEST_AUDIO_SIGNAL_H
// The audio suites' shared fixtures: the reference signal every encoded fixture in content/audio/
// carries, and a WAV writer. Issue #87.
//
// THE REFERENCE SIGNAL IS INTEGERS. tools/content/make_audio.py encodes `reference_sample` into
// content/audio/fixtures/tone.{flac,ogg,mp3}, and this header computes the same integers, so a
// lossless decode is compared sample for sample and a lossy one by its signal-to-noise ratio — on
// every platform, with no reference PCM committed and no libm in the comparison. Change the two
// together or not at all.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace cy::test::audio_signal {

inline constexpr std::uint32_t kFixtureRate = 44100;
/// tone.flac is one second; tone.ogg and tone.mp3 are six.
inline constexpr std::uint64_t kShortFrames = 44100;
inline constexpr std::uint64_t kLongFrames = std::uint64_t{6} * 44100U;

/// A parabolic approximation of a sine: a positive hump, then a negative one.
[[nodiscard]] inline std::int32_t hump(std::uint64_t frame, std::int64_t period,
                                       std::int64_t amplitude) noexcept {
    const std::int64_t half = period / 2;
    auto phase = static_cast<std::int64_t>(frame % static_cast<std::uint64_t>(period));
    std::int64_t sign = 1;
    if (phase >= half) {
        phase -= half;
        sign = -1;
    }
    return static_cast<std::int32_t>(sign *
                                     ((4 * amplitude * phase * (half - phase)) / (half * half)));
}

/// The fixture signal's sample, a signed 16-bit value. Mirrors make_audio.py's `reference`.
[[nodiscard]] inline std::int16_t reference_sample(std::uint64_t frame,
                                                   std::uint32_t channel) noexcept {
    if (channel == 0) {
        return static_cast<std::int16_t>(hump(frame, 100, 8000) + hump(frame, 17642, 3000));
    }
    return static_cast<std::int16_t>(hump(frame, 148, 7000) + hump(frame, 26462, 3000));
}

[[nodiscard]] inline float reference_float(std::uint64_t frame, std::uint32_t channel) noexcept {
    return static_cast<float>(reference_sample(frame, channel)) * (1.0F / 32768.0F);
}

inline void put_u16(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

inline void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    put_u16(out, value & 0xFFFFU);
    put_u16(out, value >> 16U);
}

inline void put_tag(std::vector<std::uint8_t>& out, const char* tag) {
    out.insert(out.end(), tag, tag + 4);
}

/// A 16-bit PCM RIFF/WAVE of interleaved `samples`, with a `smpl` forward loop over
/// [loop_first, loop_last] (inclusive, the format's convention) when `with_loop` is set.
[[nodiscard]] inline std::vector<std::uint8_t> wav_bytes(const std::vector<std::int16_t>& samples,
                                                         std::uint32_t channels, std::uint32_t rate,
                                                         bool with_loop = false,
                                                         std::uint32_t loop_first = 0,
                                                         std::uint32_t loop_last = 0) {
    std::vector<std::uint8_t> chunks;
    put_tag(chunks, "fmt ");
    put_u32(chunks, 16);
    put_u16(chunks, 1);
    put_u16(chunks, channels);
    put_u32(chunks, rate);
    put_u32(chunks, rate * channels * 2);
    put_u16(chunks, channels * 2);
    put_u16(chunks, 16);
    if (with_loop) {
        put_tag(chunks, "smpl");
        put_u32(chunks, 36 + 24);
        const std::uint32_t header[9] = {0, 0, 1000000000U / rate, 60, 0, 0, 0, 1, 0};
        for (const std::uint32_t value : header) {
            put_u32(chunks, value);
        }
        const std::uint32_t loop[6] = {0, 0, loop_first, loop_last, 0, 0};
        for (const std::uint32_t value : loop) {
            put_u32(chunks, value);
        }
    }
    put_tag(chunks, "data");
    put_u32(chunks, static_cast<std::uint32_t>(samples.size() * 2));
    for (const std::int16_t sample : samples) {
        put_u16(chunks, static_cast<std::uint16_t>(sample));
    }
    std::vector<std::uint8_t> out;
    put_tag(out, "RIFF");
    put_u32(out, static_cast<std::uint32_t>(4 + chunks.size()));
    put_tag(out, "WAVE");
    out.insert(out.end(), chunks.begin(), chunks.end());
    return out;
}

/// The reference signal as interleaved stereo 16-bit samples, `frames` long.
[[nodiscard]] inline std::vector<std::int16_t> reference_samples(std::uint64_t frames) {
    std::vector<std::int16_t> out;
    out.reserve(frames * 2);
    for (std::uint64_t frame = 0; frame < frames; ++frame) {
        out.push_back(reference_sample(frame, 0));
        out.push_back(reference_sample(frame, 1));
    }
    return out;
}

/// A whole file's bytes, or none when it cannot be opened or read — which the caller's checks then
/// report, since a REQUIRE does not stop a case in this build.
[[nodiscard]] inline std::vector<std::uint8_t> read_file(const std::string& path) {
    std::vector<std::uint8_t> bytes;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return bytes;
    }
    const bool sized = std::fseek(file, 0, SEEK_END) == 0;
    const long length = sized ? std::ftell(file) : -1L;
    if (length > 0 && std::fseek(file, 0, SEEK_SET) == 0) {
        bytes.resize(static_cast<std::size_t>(length));
        if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
            bytes.clear();
        }
    }
    (void)std::fclose(file);
    return bytes;
}

}  // namespace cy::test::audio_signal

#endif  // CY_TEST_AUDIO_SIGNAL_H
