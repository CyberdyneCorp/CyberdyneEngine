// SPDX-License-Identifier: MIT
// BS.1770 loudness. See cy/servers/audio/loudness.h.

#include <cy/servers/audio/loudness.h>

#include <cy/core/math/scalar.h>
#include <cy/core/memory/array.h>

#include <cmath>

namespace cy::audio {
namespace {

/// One biquad section, direct form I, in double: a long track summed in float loses the gating's
/// precision before it reaches the end.
struct Biquad {
    f64 b0 = 1.0;
    f64 b1 = 0.0;
    f64 b2 = 0.0;
    f64 a1 = 0.0;
    f64 a2 = 0.0;
    f64 x1 = 0.0;
    f64 x2 = 0.0;
    f64 y1 = 0.0;
    f64 y2 = 0.0;

    f64 step(f64 x) noexcept {
        const f64 y = (b0 * x) + (b1 * x1) + (b2 * x2) - (a1 * y1) - (a2 * y2);
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};

/// Stage one: the head's acoustic effect, a high shelf. Coefficients from the analogue prototype
/// the standard's 48 kHz table was derived from, so any rate gets its own exact filter.
[[nodiscard]] Biquad shelf(u32 rate) noexcept {
    constexpr f64 kFrequency = 1681.974450955533;
    constexpr f64 kGainDb = 3.999843853973347;
    constexpr f64 kQ = 0.7071752369554196;
    const f64 k = std::tan(math::kPi_f64 * kFrequency / rate);
    const f64 vh = std::pow(10.0, kGainDb / 20.0);
    const f64 vb = std::pow(vh, 0.4996667741545416);
    const f64 a0 = 1.0 + (k / kQ) + (k * k);
    Biquad filter;
    filter.b0 = (vh + (vb * k / kQ) + (k * k)) / a0;
    filter.b1 = 2.0 * ((k * k) - vh) / a0;
    filter.b2 = (vh - (vb * k / kQ) + (k * k)) / a0;
    filter.a1 = 2.0 * ((k * k) - 1.0) / a0;
    filter.a2 = (1.0 - (k / kQ) + (k * k)) / a0;
    return filter;
}

/// Stage two: the revised low-frequency B-weighting, a high pass.
[[nodiscard]] Biquad high_pass(u32 rate) noexcept {
    constexpr f64 kFrequency = 38.13547087602444;
    constexpr f64 kQ = 0.5003270373238773;
    const f64 k = std::tan(math::kPi_f64 * kFrequency / rate);
    const f64 a0 = 1.0 + (k / kQ) + (k * k);
    Biquad filter;
    filter.b0 = 1.0;
    filter.b1 = -2.0;
    filter.b2 = 1.0;
    filter.a1 = 2.0 * ((k * k) - 1.0) / a0;
    filter.a2 = (1.0 - (k / kQ) + (k * k)) / a0;
    return filter;
}

[[nodiscard]] f64 block_loudness(f64 mean_square) noexcept {
    return -0.691 + (10.0 * std::log10(math::max(mean_square, 1e-30)));
}

/// The gating, over per-step energies. A block is four 100 ms steps; `steps` holds each step's
/// summed (channel-weighted) energy and `step_frames` its length.
[[nodiscard]] f32 gated_loudness(const f64* steps, u64 step_count, u64 step_frames) noexcept {
    if (step_count < 4) {
        return -200.0F;
    }
    const f64 block_frames = static_cast<f64>(step_frames) * 4.0;
    const auto block_energy = [&](u64 block) {
        return (steps[block] + steps[block + 1] + steps[block + 2] + steps[block + 3]) /
               block_frames;
    };
    const u64 blocks = step_count - 3;
    f64 absolute_sum = 0.0;
    u64 absolute_count = 0;
    for (u64 block = 0; block < blocks; ++block) {
        const f64 energy = block_energy(block);
        if (block_loudness(energy) > -70.0) {
            absolute_sum += energy;
            ++absolute_count;
        }
    }
    if (absolute_count == 0) {
        return -200.0F;
    }
    const f64 relative_gate =
        block_loudness(absolute_sum / static_cast<f64>(absolute_count)) - 10.0;
    f64 sum = 0.0;
    u64 count = 0;
    for (u64 block = 0; block < blocks; ++block) {
        const f64 energy = block_energy(block);
        const f64 loudness = block_loudness(energy);
        if (loudness > -70.0 && loudness > relative_gate) {
            sum += energy;
            ++count;
        }
    }
    return (count == 0) ? -200.0F : static_cast<f32>(block_loudness(sum / static_cast<f64>(count)));
}

}  // namespace

Expected<LoudnessMeasurement, Error> measure_loudness(const f32* frames, u64 frame_count,
                                                      u32 channels, u32 sample_rate,
                                                      Allocator& allocator) noexcept {
    LoudnessMeasurement result;
    if (frames == nullptr || frame_count == 0) {
        return result;
    }
    if (channels == 0 || channels > 2 || sample_rate == 0) {
        return fail(ErrorCode::InvalidArgument, "loudness is measured over one or two channels");
    }
    Biquad stage_one[2] = {shelf(sample_rate), shelf(sample_rate)};
    Biquad stage_two[2] = {high_pass(sample_rate), high_pass(sample_rate)};

    // 100 ms steps; the 400 ms blocks with 75 % overlap are four consecutive steps.
    const u64 step_frames = math::max<u64>(sample_rate / 10, 1);
    const u64 stored = frame_count / step_frames;
    // One energy per step: the gating's two passes need every block, and a ten-minute track is
    // 6 000 steps.
    Array<f64> steps(allocator);
    if (Status sized = steps.resize(static_cast<usize>(stored)); !sized) {
        return make_unexpected(sized.error());
    }

    f64 peak = 0.0;
    for (u64 step = 0; step < stored; ++step) {
        f64 energy = 0.0;
        for (u64 frame = step * step_frames; frame < (step + 1) * step_frames; ++frame) {
            for (u32 channel = 0; channel < channels; ++channel) {
                const f64 sample = static_cast<f64>(frames[(frame * channels) + channel]);
                peak = math::max(peak, std::fabs(sample));
                const f64 weighted = stage_two[channel].step(stage_one[channel].step(sample));
                energy += weighted * weighted;
            }
        }
        steps[step] = energy;
    }
    for (u64 index = stored * step_frames * channels; index < frame_count * channels; ++index) {
        peak = math::max(peak, static_cast<f64>(std::fabs(frames[index])));
    }
    result.integrated_lufs = gated_loudness(steps.data(), stored, step_frames);
    result.peak_dbfs = (peak <= 0.0) ? -200.0F : static_cast<f32>(20.0 * std::log10(peak));
    return result;
}

}  // namespace cy::audio
