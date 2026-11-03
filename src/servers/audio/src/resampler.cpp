// SPDX-License-Identifier: MIT
// Windowed-sinc rate conversion. See cy/servers/audio/resampler.h.

#include <cy/servers/audio/resampler.h>

#include <cy/core/math/scalar.h>

#include <cmath>
#include <cstring>
#include <numeric>

namespace cy::audio {
namespace {

constexpr u32 kTaps = 2 * Resampler::kHalfTaps;
/// Room for this many frames of input beyond the filter's span before the history compacts.
constexpr u32 kHistorySlack = 1024;

[[nodiscard]] f64 blackman(f64 x) noexcept {
    // x in [-1, 1]; zero outside.
    if (x <= -1.0 || x >= 1.0) {
        return 0.0;
    }
    const f64 t = (x + 1.0) * 0.5;
    return 0.42 - (0.5 * std::cos(2.0 * math::kPi_f64 * t)) +
           (0.08 * std::cos(4.0 * math::kPi_f64 * t));
}

[[nodiscard]] f64 sinc(f64 x) noexcept {
    if (std::fabs(x) < 1e-12) {
        return 1.0;
    }
    const f64 angle = math::kPi_f64 * x;
    return std::sin(angle) / angle;
}

}  // namespace

Resampler::Resampler(Allocator& allocator) noexcept : table_(allocator), history_(allocator) {}

Status Resampler::configure(u32 input_rate, u32 output_rate, u32 channels) noexcept {
    if (input_rate == 0 || output_rate == 0 || channels == 0 || channels > 2) {
        return fail(ErrorCode::InvalidArgument,
                    "a resampler needs two non-zero rates and one or two channels");
    }
    input_rate_ = input_rate;
    output_rate_ = output_rate;
    channels_ = channels;
    const u64 divisor = std::gcd(static_cast<u64>(input_rate), static_cast<u64>(output_rate));
    denominator_ = output_rate / divisor;
    const u64 numerator = input_rate / divisor;
    step_whole_ = numerator / denominator_;
    step_numerator_ = numerator % denominator_;
    if (!passthrough()) {
        if (Status sized = table_.allocate(static_cast<usize>(kPhases + 1) * kTaps); !sized) {
            return sized;
        }
        build_table();
        if (Status sized = history_.allocate(static_cast<usize>(kTaps + kHistorySlack) * channels);
            !sized) {
            return sized;
        }
    } else {
        table_.release();
        history_.release();
    }
    reset();
    return ok();
}

void Resampler::build_table() noexcept {
    const f64 cutoff = 0.95 * math::min(1.0, static_cast<f64>(output_rate_) / input_rate_);
    for (u32 phase = 0; phase <= kPhases; ++phase) {
        const f64 fraction = static_cast<f64>(phase) / kPhases;
        f32* row = table_.data() + (static_cast<usize>(phase) * kTaps);
        f64 sum = 0.0;
        f64 taps[kTaps];
        for (u32 tap = 0; tap < kTaps; ++tap) {
            // Tap `tap` reads input frame floor(position) - kHalfTaps + 1 + tap.
            const f64 distance = static_cast<f64>(tap) - (kHalfTaps - 1) - fraction;
            taps[tap] = cutoff * sinc(cutoff * distance) * blackman(distance / kHalfTaps);
            sum += taps[tap];
        }
        for (u32 tap = 0; tap < kTaps; ++tap) {
            row[tap] = static_cast<f32>(taps[tap] / sum);
        }
    }
}

void Resampler::reset() noexcept {
    total_input_ = 0;
    total_output_ = 0;
    numerator_ = 0;
    if (passthrough()) {
        return;
    }
    // kHalfTaps - 1 frames of silence before the first: the taps at the first output position that
    // reach back before frame zero read them, which is what keeps the output time-aligned.
    held_ = kHalfTaps - 1;
    position_ = kHalfTaps - 1;
    std::memset(history_.data(), 0, history_.size() * sizeof(f32));
}

u32 Resampler::max_output(u32 input_frames) const noexcept {
    if (passthrough()) {
        return input_frames;
    }
    // Every output whose position falls before the end of what is held, plus one for rounding.
    const u64 span = (static_cast<u64>(input_frames) + kTaps) * output_rate_;
    return static_cast<u32>((span / input_rate_) + 2);
}

u64 Resampler::convert_frame(u64 frame, u32 from_rate, u32 to_rate) noexcept {
    if (from_rate == to_rate || from_rate == 0) {
        return frame;
    }
    return ((frame * to_rate) + (from_rate / 2)) / from_rate;
}

void Resampler::append(const f32* input, u32 frames) noexcept {
    const usize capacity = history_.size() / channels_;
    if (held_ + frames > capacity) {
        // Compact: drop the frames no future output can reach. A large downsampling step can put
        // the next position past everything held, so the drop is bounded by what there is.
        const u64 drop = math::min<u64>(position_ - (kHalfTaps - 1), held_);
        const u64 keep = held_ - drop;
        std::memmove(history_.data(), history_.data() + (drop * channels_),
                     static_cast<usize>(keep) * channels_ * sizeof(f32));
        held_ = keep;
        position_ -= drop;
    }
    std::memcpy(history_.data() + (held_ * channels_), input,
                static_cast<usize>(frames) * channels_ * sizeof(f32));
    held_ += frames;
}

u32 Resampler::produce(f32* output) noexcept {
    u32 written = 0;
    // An output needs frames up to floor(position) + kHalfTaps; the last held frame is held_ - 1.
    while (position_ + kHalfTaps < held_) {
        const u64 scaled = numerator_ * kPhases;
        const u64 phase = scaled / denominator_;
        const f32 blend = static_cast<f32>(scaled % denominator_) / static_cast<f32>(denominator_);
        const f32* row_a = table_.data() + (phase * kTaps);
        const f32* row_b = row_a + kTaps;
        const f32* frames = history_.data() + ((position_ - (kHalfTaps - 1)) * channels_);
        for (u32 channel = 0; channel < channels_; ++channel) {
            f32 sum_a = 0.0F;
            f32 sum_b = 0.0F;
            for (u32 tap = 0; tap < kTaps; ++tap) {
                const f32 sample = frames[(static_cast<usize>(tap) * channels_) + channel];
                sum_a += sample * row_a[tap];
                sum_b += sample * row_b[tap];
            }
            output[(static_cast<usize>(written) * channels_) + channel] =
                sum_a + ((sum_b - sum_a) * blend);
        }
        ++written;
        position_ += step_whole_;
        numerator_ += step_numerator_;
        if (numerator_ >= denominator_) {
            numerator_ -= denominator_;
            ++position_;
        }
    }
    return written;
}

u32 Resampler::process(const f32* input, u32 input_frames, f32* output) noexcept {
    total_input_ += input_frames;
    if (passthrough()) {
        std::memcpy(output, input, static_cast<usize>(input_frames) * channels_ * sizeof(f32));
        total_output_ += input_frames;
        return input_frames;
    }
    u32 written = 0;
    const u32 capacity = static_cast<u32>(history_.size() / channels_) - kTaps;
    u32 consumed = 0;
    while (consumed < input_frames) {
        const u32 chunk = math::min(input_frames - consumed, capacity);
        append(input + (static_cast<usize>(consumed) * channels_), chunk);
        consumed += chunk;
        written += produce(output + (static_cast<usize>(written) * channels_));
    }
    total_output_ += written;
    return written;
}

u32 Resampler::drain(f32* output) noexcept {
    if (passthrough()) {
        return 0;
    }
    // The output the whole input owes: ceil(total_input * out / in).
    const u64 owed = ((total_input_ * output_rate_) + input_rate_ - 1) / input_rate_;
    if (total_output_ >= owed) {
        return 0;
    }
    const f32 silence[kTaps * 2] = {};
    const u64 before = total_output_;
    u32 written = 0;
    // Push silence a few frames at a time until every owed output is out, then stop exactly there.
    while (total_output_ < owed) {
        append(silence, 1);
        const u32 produced = produce(output + (static_cast<usize>(written) * channels_));
        written += produced;
        total_output_ += produced;
    }
    const auto keep = static_cast<u32>(owed - before);
    total_output_ = owed;
    return keep;
}

usize Resampler::resident_bytes() const noexcept {
    return table_.bytes() + history_.bytes();
}

void convert_channels(const f32* in, u32 input_channels, f32* out, u32 output_channels,
                      u32 frames) noexcept {
    if (input_channels == output_channels) {
        std::memmove(out, in, static_cast<usize>(frames) * input_channels * sizeof(f32));
        return;
    }
    if (input_channels == 1 && output_channels == 2) {
        // Backwards, so the conversion may run in place.
        for (u32 frame = frames; frame > 0; --frame) {
            const f32 sample = in[frame - 1];
            out[(static_cast<usize>(frame) - 1) * 2] = sample;
            out[((static_cast<usize>(frame) - 1) * 2) + 1] = sample;
        }
        return;
    }
    // Stereo to mono: the average, which keeps a centred sound at its level.
    for (u32 frame = 0; frame < frames; ++frame) {
        const usize at = static_cast<usize>(frame) * 2;
        out[frame] = 0.5F * (in[at] + in[at + 1]);
    }
}

}  // namespace cy::audio
