// The effect chain: biquads, dynamics, a delay line, and the analysis tap. M8.b task 10.2.

#include <cy/audio/effects.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cy::audio {
namespace {

constexpr u32 kMaxChannels = 2;
/// One second of delay per instance at 48 kHz. A delay longer than that is a reverb, and reverb is
/// its own effect.
constexpr u32 kDelayCapacity = 48000;

[[nodiscard]] f32 clampf(f32 value, f32 low, f32 high) noexcept {
    return std::clamp(value, low, high);
}

[[nodiscard]] f32 approach(f32 current, f32 target, f32 alpha) noexcept {
    return current + ((target - current) * alpha);
}

/// One biquad's coefficients, from the Audio EQ Cookbook's formulas — the ones every audio engine
/// uses, so a sound designer's intuition about Q and shelf gain transfers.
struct Biquad {
    f32 b0 = 1.0F;
    f32 b1 = 0.0F;
    f32 b2 = 0.0F;
    f32 a1 = 0.0F;
    f32 a2 = 0.0F;
};

[[nodiscard]] Biquad make_biquad(EffectKind kind, const EffectParameters& parameters,
                                 u32 sample_rate) noexcept {
    Biquad biquad;
    const f32 rate = (sample_rate > 0) ? static_cast<f32>(sample_rate) : 48000.0F;
    const f32 frequency = clampf(parameters.frequency, 10.0F, rate * 0.45F);
    const f32 q = (parameters.q > 0.01F) ? parameters.q : 0.707F;
    const f32 omega = 6.2831853F * frequency / rate;
    const f32 sine = std::sin(omega);
    const f32 cosine = std::cos(omega);
    const f32 alpha = sine / (2.0F * q);
    const f32 amplitude = std::pow(10.0F, parameters.decibels / 40.0F);

    f32 b0 = 1.0F;
    f32 b1 = 0.0F;
    f32 b2 = 0.0F;
    f32 a0 = 1.0F;
    f32 a1 = 0.0F;
    f32 a2 = 0.0F;
    switch (kind) {
        case EffectKind::LowPass:
            b0 = (1.0F - cosine) * 0.5F;
            b1 = 1.0F - cosine;
            b2 = b0;
            a0 = 1.0F + alpha;
            a1 = -2.0F * cosine;
            a2 = 1.0F - alpha;
            break;
        case EffectKind::HighPass:
            b0 = (1.0F + cosine) * 0.5F;
            b1 = -(1.0F + cosine);
            b2 = b0;
            a0 = 1.0F + alpha;
            a1 = -2.0F * cosine;
            a2 = 1.0F - alpha;
            break;
        case EffectKind::BandPass:
            b0 = alpha;
            b1 = 0.0F;
            b2 = -alpha;
            a0 = 1.0F + alpha;
            a1 = -2.0F * cosine;
            a2 = 1.0F - alpha;
            break;
        case EffectKind::LowShelf: {
            const f32 root = 2.0F * std::sqrt(amplitude) * alpha;
            b0 = amplitude * ((amplitude + 1.0F) - ((amplitude - 1.0F) * cosine) + root);
            b1 = 2.0F * amplitude * ((amplitude - 1.0F) - ((amplitude + 1.0F) * cosine));
            b2 = amplitude * ((amplitude + 1.0F) - ((amplitude - 1.0F) * cosine) - root);
            a0 = (amplitude + 1.0F) + ((amplitude - 1.0F) * cosine) + root;
            a1 = -2.0F * ((amplitude - 1.0F) + ((amplitude + 1.0F) * cosine));
            a2 = (amplitude + 1.0F) + ((amplitude - 1.0F) * cosine) - root;
            break;
        }
        case EffectKind::HighShelf: {
            const f32 root = 2.0F * std::sqrt(amplitude) * alpha;
            b0 = amplitude * ((amplitude + 1.0F) + ((amplitude - 1.0F) * cosine) + root);
            b1 = -2.0F * amplitude * ((amplitude - 1.0F) + ((amplitude + 1.0F) * cosine));
            b2 = amplitude * ((amplitude + 1.0F) + ((amplitude - 1.0F) * cosine) - root);
            a0 = (amplitude + 1.0F) - ((amplitude - 1.0F) * cosine) + root;
            a1 = 2.0F * ((amplitude - 1.0F) - ((amplitude + 1.0F) * cosine));
            a2 = (amplitude + 1.0F) - ((amplitude - 1.0F) * cosine) - root;
            break;
        }
        case EffectKind::ParametricEq:
        default:
            b0 = 1.0F + (alpha * amplitude);
            b1 = -2.0F * cosine;
            b2 = 1.0F - (alpha * amplitude);
            a0 = 1.0F + (alpha / amplitude);
            a1 = -2.0F * cosine;
            a2 = 1.0F - (alpha / amplitude);
            break;
    }
    const f32 inverse = (std::fabs(a0) > 1e-9F) ? (1.0F / a0) : 1.0F;
    biquad.b0 = b0 * inverse;
    biquad.b1 = b1 * inverse;
    biquad.b2 = b2 * inverse;
    biquad.a1 = a1 * inverse;
    biquad.a2 = a2 * inverse;
    return biquad;
}

[[nodiscard]] bool is_filter(EffectKind kind) noexcept {
    return kind == EffectKind::LowPass || kind == EffectKind::HighPass ||
           kind == EffectKind::BandPass || kind == EffectKind::LowShelf ||
           kind == EffectKind::HighShelf || kind == EffectKind::ParametricEq;
}

}  // namespace

const char* effect_kind_name(EffectKind kind) noexcept {
    switch (kind) {
        case EffectKind::Gain:
            return "gain";
        case EffectKind::ParametricEq:
            return "parametric-eq";
        case EffectKind::LowPass:
            return "low-pass";
        case EffectKind::HighPass:
            return "high-pass";
        case EffectKind::BandPass:
            return "band-pass";
        case EffectKind::LowShelf:
            return "low-shelf";
        case EffectKind::HighShelf:
            return "high-shelf";
        case EffectKind::Compressor:
            return "compressor";
        case EffectKind::Limiter:
            return "limiter";
        case EffectKind::Gate:
            return "gate";
        case EffectKind::ReverbAlgorithmic:
            return "reverb-algorithmic";
        case EffectKind::ReverbConvolution:
            return "reverb-convolution";
        case EffectKind::Delay:
            return "delay";
        case EffectKind::Chorus:
            return "chorus";
        case EffectKind::Flanger:
            return "flanger";
        case EffectKind::Phaser:
            return "phaser";
        case EffectKind::Distortion:
            return "distortion";
        case EffectKind::PitchShift:
            return "pitch-shift";
        case EffectKind::StereoWidener:
            return "stereo-widener";
        case EffectKind::Panner:
            return "panner";
        case EffectKind::AnalysisTap:
            return "analysis-tap";
        case EffectKind::Custom:
            return "custom";
        case EffectKind::Count:
            break;
    }
    return "unknown";
}

bool effect_is_implemented(EffectKind kind) noexcept {
    switch (kind) {
        // PROCESSING SAMPLES.
        case EffectKind::Gain:
        case EffectKind::ParametricEq:
        case EffectKind::LowPass:
        case EffectKind::HighPass:
        case EffectKind::BandPass:
        case EffectKind::LowShelf:
        case EffectKind::HighShelf:
        case EffectKind::Compressor:
        case EffectKind::Limiter:
        case EffectKind::Gate:
        case EffectKind::Delay:
        case EffectKind::Distortion:
        case EffectKind::StereoWidener:
        case EffectKind::Panner:
        case EffectKind::AnalysisTap:
        case EffectKind::Custom:
            return true;
        // DECLARED, WITH A LATENCY, AND PASSING AUDIO THROUGH. See the header: queryable rather
        // than silent, so a mixer window can grey them out instead of a sound designer wondering
        // why the chorus does nothing.
        case EffectKind::ReverbAlgorithmic:
        case EffectKind::ReverbConvolution:
        case EffectKind::Chorus:
        case EffectKind::Flanger:
        case EffectKind::Phaser:
        case EffectKind::PitchShift:
        case EffectKind::Count:
            break;
    }
    return false;
}

u32 effect_latency_samples(EffectKind kind, u32 sample_rate) noexcept {
    const f32 rate = (sample_rate > 0) ? static_cast<f32>(sample_rate) : 48000.0F;
    switch (kind) {
        case EffectKind::ReverbConvolution:
            // Partitioned convolution's first partition: what the engine must compensate for.
            return static_cast<u32>(rate * 0.0053F);  // 256 samples at 48 kHz
        case EffectKind::PitchShift:
            return static_cast<u32>(rate * 0.0213F);  // one 1024-sample window
        case EffectKind::Limiter:
            // A look-ahead limiter is the only dynamics processor with latency, and it has it for a
            // reason: it cannot catch a transient it has not seen.
            return static_cast<u32>(rate * 0.0015F);
        default:
            return 0;
    }
}

EffectChain::EffectChain(Allocator& allocator, u32 sample_rate) noexcept
    : effects_(allocator),
      states_(allocator),
      delay_lines_(allocator),
      sample_rate_((sample_rate > 0) ? sample_rate : 48000U) {}

Status EffectChain::add(const EffectInstance& effect) noexcept {
    EffectInstance instance = effect;
    instance.implemented = effect_is_implemented(effect.kind);
    instance.latency_samples = effect_latency_samples(effect.kind, sample_rate_);
    instance.current = instance.target;
    if (Status pushed = effects_.push_back(instance); !pushed) {
        return pushed;
    }
    if (Status pushed = states_.push_back(State{}); !pushed) {
        effects_.pop_back();
        return pushed;
    }
    // A DELAY LINE ONLY FOR THE EFFECTS THAT DELAY, allocated once here rather than on a parameter
    // change: an allocation on the audio thread is the one allocation that is unacceptable. Giving
    // every gain and filter a line would cost a megabyte per chain and a millisecond of zeroing,
    // which is what the effects suite measured before this was a condition.
    if (instance.kind == EffectKind::Delay) {
        const usize base = delay_lines_.size();
        const usize samples = static_cast<usize>(kDelayCapacity) * kMaxChannels;
        if (Status sized = delay_lines_.resize(base + samples); !sized) {
            effects_.pop_back();
            states_.pop_back();
            return sized;
        }
        for (usize index = base; index < delay_lines_.size(); ++index) {
            delay_lines_[index] = 0.0F;
        }
        states_[states_.size() - 1].delay_base = base;
    }
    return ok();
}

Status EffectChain::remove(Name name) noexcept {
    for (usize index = 0; index < effects_.size(); ++index) {
        if (effects_[index].name == name) {
            effects_.remove_unordered(index);
            states_.remove_unordered(index);
            return ok();
        }
    }
    return make_unexpected(Error{ErrorCode::NotFound, "no effect with that name", 0});
}

EffectInstance* EffectChain::find(Name name) noexcept {
    for (EffectInstance& effect : effects_.span()) {
        if (effect.name == name) {
            return &effect;
        }
    }
    return nullptr;
}

Status EffectChain::set_parameters(Name name, const EffectParameters& parameters) noexcept {
    EffectInstance* effect = find(name);
    if (effect == nullptr) {
        return make_unexpected(Error{ErrorCode::NotFound, "no effect with that name", 0});
    }
    // THE TARGET, not the value. `advance` walks toward it, because a jumped cutoff is a click.
    effect->target = parameters;
    return ok();
}

void EffectChain::advance(f32 dt) noexcept {
    if (dt <= 0.0F) {
        return;
    }
    const f32 alpha =
        (interpolation_seconds > 0.0F) ? (1.0F - std::pow(0.5F, dt / interpolation_seconds)) : 1.0F;
    for (EffectInstance& effect : effects_.span()) {
        EffectParameters& current = effect.current;
        const EffectParameters& target = effect.target;
        current.gain = approach(current.gain, target.gain, alpha);
        current.frequency = approach(current.frequency, target.frequency, alpha);
        current.q = approach(current.q, target.q, alpha);
        current.decibels = approach(current.decibels, target.decibels, alpha);
        current.threshold = approach(current.threshold, target.threshold, alpha);
        current.ratio = approach(current.ratio, target.ratio, alpha);
        current.time_seconds = approach(current.time_seconds, target.time_seconds, alpha);
        current.feedback = approach(current.feedback, target.feedback, alpha);
        current.mix = approach(current.mix, target.mix, alpha);
        current.pan = approach(current.pan, target.pan, alpha);
        current.width = approach(current.width, target.width, alpha);
        // Attack and release are NOT interpolated: they are time constants, and moving them
        // smoothly would make a compressor's behaviour depend on when it was last reconfigured.
        current.attack_seconds = target.attack_seconds;
        current.release_seconds = target.release_seconds;
    }
}

u32 EffectChain::latency_samples() const noexcept {
    u32 total = 0;
    for (const EffectInstance& effect : effects_.span()) {
        if (!effect.bypassed) {
            total += effect.latency_samples;
        }
    }
    return total;
}

Status EffectChain::process(f32* samples, u32 frames, u32 channels, ChainReport& report) noexcept {
    report = ChainReport{};
    report.effects = static_cast<u32>(effects_.size());
    if (samples == nullptr || frames == 0 || channels == 0) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "an empty block", 0});
    }
    if (channels > kMaxChannels) {
        return make_unexpected(
            Error{ErrorCode::Unsupported, "this chain processes mono and stereo", 0});
    }

    for (usize index = 0; index < effects_.size(); ++index) {
        EffectInstance& effect = effects_[index];
        State& state = states_[index];
        if (effect.bypassed) {
            ++report.bypassed;
            continue;
        }
        if (!effect.implemented) {
            // PASSED THROUGH, AND COUNTED. The audio is unchanged and the report says why.
            ++report.passthrough;
            continue;
        }
        ++report.processed;
        const EffectParameters& parameters = effect.current;

        if (is_filter(effect.kind)) {
            const Biquad biquad = make_biquad(effect.kind, parameters, sample_rate_);
            for (u32 frame = 0; frame < frames; ++frame) {
                for (u32 channel = 0; channel < channels; ++channel) {
                    const usize slot = (static_cast<usize>(frame) * channels) + channel;
                    const f32 input = samples[slot];
                    const f32 output = (biquad.b0 * input) + (biquad.b1 * state.x1[channel]) +
                                       (biquad.b2 * state.x2[channel]) -
                                       (biquad.a1 * state.y1[channel]) -
                                       (biquad.a2 * state.y2[channel]);
                    state.x2[channel] = state.x1[channel];
                    state.x1[channel] = input;
                    state.y2[channel] = state.y1[channel];
                    state.y1[channel] = output;
                    samples[slot] = output;
                }
            }
            continue;
        }

        switch (effect.kind) {
            case EffectKind::Gain: {
                for (u32 slot = 0; slot < frames * channels; ++slot) {
                    samples[slot] *= parameters.gain;
                }
                break;
            }
            case EffectKind::Compressor:
            case EffectKind::Limiter:
            case EffectKind::Gate: {
                const f32 attack = (parameters.attack_seconds > 0.0F)
                                       ? std::exp(-1.0F / (parameters.attack_seconds *
                                                           static_cast<f32>(sample_rate_)))
                                       : 0.0F;
                const f32 release = (parameters.release_seconds > 0.0F)
                                        ? std::exp(-1.0F / (parameters.release_seconds *
                                                            static_cast<f32>(sample_rate_)))
                                        : 0.0F;
                for (u32 frame = 0; frame < frames; ++frame) {
                    for (u32 channel = 0; channel < channels; ++channel) {
                        const usize slot = (static_cast<usize>(frame) * channels) + channel;
                        const f32 level = std::fabs(samples[slot]);
                        // A one-pole envelope with separate attack and release: the shape every
                        // dynamics processor is built on.
                        const f32 coefficient =
                            (level > state.envelope[channel]) ? attack : release;
                        state.envelope[channel] = (state.envelope[channel] * coefficient) +
                                                  (level * (1.0F - coefficient));
                        const f32 envelope = state.envelope[channel];

                        f32 gain = 1.0F;
                        if (effect.kind == EffectKind::Gate) {
                            gain = (envelope < parameters.threshold) ? 0.0F : 1.0F;
                        } else if (envelope > parameters.threshold && envelope > 1e-6F) {
                            const f32 over = envelope / parameters.threshold;
                            // A limiter is a compressor at an effectively infinite ratio, which is
                            // the one line of difference between the two.
                            const f32 ratio = (effect.kind == EffectKind::Limiter)
                                                  ? 1000.0F
                                                  : std::max(parameters.ratio, 1.0F);
                            gain = std::pow(over, (1.0F / ratio) - 1.0F);
                        }
                        samples[slot] *= gain * parameters.gain;
                    }
                }
                break;
            }
            case EffectKind::Delay: {
                if (state.delay_base == kNoDelayLine) {
                    break;
                }
                const usize base = state.delay_base;
                const auto delay_frames = static_cast<u32>(
                    clampf(parameters.time_seconds * static_cast<f32>(sample_rate_), 1.0F,
                           static_cast<f32>(kDelayCapacity - 1)));
                for (u32 frame = 0; frame < frames; ++frame) {
                    for (u32 channel = 0; channel < channels; ++channel) {
                        const usize slot = (static_cast<usize>(frame) * channels) + channel;
                        const u32 write = (state.cursor + frame) % kDelayCapacity;
                        const u32 read = (write + kDelayCapacity - delay_frames) % kDelayCapacity;
                        const usize line = base + (static_cast<usize>(channel) * kDelayCapacity);
                        const f32 delayed = delay_lines_[line + read];
                        const f32 input = samples[slot];
                        delay_lines_[line + write] = input + (delayed * parameters.feedback);
                        samples[slot] =
                            (input * (1.0F - parameters.mix)) + (delayed * parameters.mix);
                    }
                }
                state.cursor = (state.cursor + frames) % kDelayCapacity;
                break;
            }
            case EffectKind::Distortion: {
                for (u32 slot = 0; slot < frames * channels; ++slot) {
                    // A soft clip: `tanh` rather than a hard clamp, because a hard clamp's
                    // harmonics are the ones that sound like a broken speaker rather than like
                    // distortion.
                    const f32 driven = samples[slot] * (1.0F + (parameters.gain * 9.0F));
                    const f32 shaped = std::tanh(driven);
                    samples[slot] =
                        (samples[slot] * (1.0F - parameters.mix)) + (shaped * parameters.mix);
                }
                break;
            }
            case EffectKind::Panner: {
                if (channels < 2) {
                    break;
                }
                // Constant power, the same law `cy::audio::pan_stereo` uses, so a panner effect and
                // a spatialised voice agree about what centre sounds like.
                const f32 angle =
                    (clampf(parameters.pan, -1.0F, 1.0F) + 1.0F) * 0.25F * std::numbers::pi_v<f32>;
                const f32 left = std::cos(angle);
                const f32 right = std::sin(angle);
                for (u32 frame = 0; frame < frames; ++frame) {
                    const usize slot = static_cast<usize>(frame) * channels;
                    samples[slot] *= left;
                    samples[slot + 1] *= right;
                }
                break;
            }
            case EffectKind::StereoWidener: {
                if (channels < 2) {
                    break;
                }
                for (u32 frame = 0; frame < frames; ++frame) {
                    const usize slot = static_cast<usize>(frame) * channels;
                    const f32 mid = (samples[slot] + samples[slot + 1]) * 0.5F;
                    const f32 side = (samples[slot] - samples[slot + 1]) * 0.5F * parameters.width;
                    samples[slot] = mid + side;
                    samples[slot + 1] = mid - side;
                }
                break;
            }
            case EffectKind::AnalysisTap: {
                analysis_ = AnalysisResult{};
                f32 sum = 0.0F;
                for (u32 slot = 0; slot < frames * channels; ++slot) {
                    const f32 value = std::fabs(samples[slot]);
                    analysis_.peak = (value > analysis_.peak) ? value : analysis_.peak;
                    sum += samples[slot] * samples[slot];
                }
                analysis_.rms = std::sqrt(sum / static_cast<f32>(frames * channels));
                // Four coarse bands from a cascade of one-pole splits. Not an FFT: a meter is what
                // a mixer window draws, and an FFT per bus per block is not free.
                f32 low = 0.0F;
                f32 previous = 0.0F;
                for (u32 frame = 0; frame < frames; ++frame) {
                    const f32 value = samples[static_cast<usize>(frame) * channels];
                    low = (low * 0.9F) + (value * 0.1F);
                    const f32 high = value - low;
                    analysis_.bands[0] += low * low;
                    analysis_.bands[3] += high * high;
                    analysis_.bands[1] += (low - previous) * (low - previous);
                    analysis_.bands[2] += (high * 0.5F) * (high * 0.5F);
                    previous = low;
                }
                for (f32& band : analysis_.bands) {
                    band = std::sqrt(band / static_cast<f32>(frames));
                }
                break;
            }
            case EffectKind::Custom: {
                if (effect.custom != nullptr) {
                    effect.custom(samples, frames, channels, parameters, effect.custom_user);
                }
                break;
            }
            default:
                break;
        }
    }
    report.latency_samples = latency_samples();
    return ok();
}

}  // namespace cy::audio
