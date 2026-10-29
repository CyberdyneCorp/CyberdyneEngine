// SPDX-License-Identifier: MIT
#pragma once
// Sample-rate and channel conversion: what a stream applies as it decodes, and what the importer
// applies offline. Issue #87.
//
// `audio` lists sample-rate and channel conversion among what a backend may do, and the engine does
// it here instead for one reason: it has to happen in the stream's DECODE JOB, chunk by chunk, with
// state carried across chunks, at a cost the engine can measure — and the import of a sound has to
// produce the same samples as a stream of the same source would. One implementation serves both.
//
// THE FILTER. A windowed sinc, 32 taps (16 either side), Blackman-windowed, its cutoff at 95 % of
// the lower of the two Nyquist frequencies so a downsample does not alias. 256 phases with linear
// interpolation between neighbours; every phase's taps are normalised to sum to one, so a constant
// signal comes out exactly constant. The position is RATIONAL — an integer frame plus a numerator
// over `output_rate / gcd` — so a 44.1 kHz stream played for an hour at 48 kHz has not drifted by a
// fraction of a frame.
//
// EQUAL RATES ARE A COPY. A 48 kHz track on a 48 kHz device is passed through bit for bit, which is
// what makes a lossless decode exact all the way to the ring buffer.
//
// TIME ALIGNMENT. Output frame `n` is the input at `n * input_rate / output_rate` exactly — the
// filter's own delay is compensated by treating the frames before the first as silence — so a loop
// point or a seek position converts by the same ratio and lands on the same sound.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/servers/audio/buffer.h>

namespace cy::audio {

/// Stateful rate conversion of interleaved float frames.
class Resampler {
public:
    /// Taps either side of the output position.
    static constexpr u32 kHalfTaps = 16;
    static constexpr u32 kPhases = 256;

    explicit Resampler(Allocator& allocator) noexcept;

    /// Allocate for a conversion and clear all state. Allocates; never call it from the audio
    /// thread.
    [[nodiscard]] Status configure(u32 input_rate, u32 output_rate, u32 channels) noexcept;

    /// Forget every input frame: the next one pushed is frame zero again. What a seek does.
    void reset() noexcept;

    [[nodiscard]] bool passthrough() const noexcept { return input_rate_ == output_rate_; }
    [[nodiscard]] u32 input_rate() const noexcept { return input_rate_; }
    [[nodiscard]] u32 output_rate() const noexcept { return output_rate_; }
    [[nodiscard]] u32 channels() const noexcept { return channels_; }

    /// The most output frames `input_frames` more input can release, so a caller sizes its buffer.
    [[nodiscard]] u32 max_output(u32 input_frames) const noexcept;

    /// Convert `input_frames` frames from `input` into `output`, which holds at least
    /// `max_output(input_frames)` frames. All input is consumed. Returns the frames written.
    [[nodiscard]] u32 process(const f32* input, u32 input_frames, f32* output) noexcept;

    /// The input has ended: write the frames still held back by the filter's look-ahead, so the
    /// total output is exactly `ceil(total input * output_rate / input_rate)` frames. `output`
    /// holds at least `max_output(kHalfTaps)` frames.
    [[nodiscard]] u32 drain(f32* output) noexcept;

    /// Bytes held: the phase table and the history.
    [[nodiscard]] usize resident_bytes() const noexcept;

    /// The output frame an input frame maps to, rounded to nearest. For loop points and seeks.
    [[nodiscard]] static u64 convert_frame(u64 frame, u32 from_rate, u32 to_rate) noexcept;

private:
    [[nodiscard]] u32 produce(f32* output) noexcept;
    void append(const f32* input, u32 frames) noexcept;
    void build_table() noexcept;

    AudioBuffer<f32> table_;    ///< (kPhases + 1) rows of 2 * kHalfTaps taps
    AudioBuffer<f32> history_;  ///< interleaved input frames, oldest first
    u32 input_rate_ = 0;
    u32 output_rate_ = 0;
    u32 channels_ = 0;
    /// The input step per output frame, as `step_whole + step_numerator / denominator_`.
    u64 step_whole_ = 0;
    u64 step_numerator_ = 0;
    u64 denominator_ = 1;
    /// The next output's position in history frames, as `position_ + numerator_ / denominator_`.
    u64 position_ = 0;
    u64 numerator_ = 0;
    /// Frames currently in `history_`.
    u64 held_ = 0;
    /// Input frames pushed since the last reset, and output frames written.
    u64 total_input_ = 0;
    u64 total_output_ = 0;
};

/// Convert interleaved frames between channel counts: mono to stereo duplicates, stereo to mono
/// averages, equal counts copy. `out` holds `frames * output_channels` floats. Only 1 and 2 are
/// supported; the cooked format holds nothing wider.
void convert_channels(const f32* in, u32 input_channels, f32* out, u32 output_channels,
                      u32 frames) noexcept;

}  // namespace cy::audio
