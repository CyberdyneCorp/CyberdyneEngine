// The effect chain: filters, dynamics, delay, metering, parameters and latency. M8.b task 10.2.

#include <cy/audio/effects.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cmath>

using namespace cy;
using namespace cy::audio;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Audio);
}

[[nodiscard]] EffectInstance effect_of(EffectKind kind, const char* name) noexcept {
    EffectInstance effect;
    effect.kind = kind;
    effect.name = Name::intern(name);
    return effect;
}

/// A block of a sine at `frequency`, interleaved stereo.
void fill_sine(f32* samples, u32 frames, u32 channels, f32 frequency, u32 rate) noexcept {
    for (u32 frame = 0; frame < frames; ++frame) {
        const f32 value =
            std::sin(6.2831853F * frequency * static_cast<f32>(frame) / static_cast<f32>(rate));
        for (u32 channel = 0; channel < channels; ++channel) {
            samples[(frame * channels) + channel] = value;
        }
    }
}

[[nodiscard]] f32 rms_of(const f32* samples, u32 count) noexcept {
    f32 sum = 0.0F;
    for (u32 index = 0; index < count; ++index) {
        sum += samples[index] * samples[index];
    }
    return std::sqrt(sum / static_cast<f32>(count));
}

}  // namespace

CY_TEST_CASE("audio_effects: a low pass keeps the low tone and removes the high one") {
    EffectChain chain(allocator(), 48000);
    EffectInstance low_pass = effect_of(EffectKind::LowPass, "lpf");
    low_pass.target.frequency = 500.0F;
    CY_REQUIRE(chain.add(low_pass).has_value());

    constexpr u32 kFrames = 512;
    f32 low[kFrames * 2];
    f32 high[kFrames * 2];
    fill_sine(low, kFrames, 2, 100.0F, 48000);
    fill_sine(high, kFrames, 2, 8000.0F, 48000);

    ChainReport report;
    CY_REQUIRE(chain.process(low, kFrames, 2, report).has_value());
    const f32 low_energy = rms_of(low, kFrames * 2);

    EffectChain second(allocator(), 48000);
    CY_REQUIRE(second.add(low_pass).has_value());
    CY_REQUIRE(second.process(high, kFrames, 2, report).has_value());
    const f32 high_energy = rms_of(high, kFrames * 2);

    CY_CHECK_GT(low_energy, 0.5F);
    CY_CHECK_LT(high_energy, low_energy * 0.2F);
    CY_CHECK_EQ(report.processed, 1U);
}

CY_TEST_CASE("audio_effects: gain, a limiter and a gate each do what their name says") {
    EffectChain chain(allocator(), 48000);
    EffectInstance gain = effect_of(EffectKind::Gain, "gain");
    gain.target.gain = 0.5F;
    CY_REQUIRE(chain.add(gain).has_value());

    constexpr u32 kFrames = 64;
    f32 block[kFrames * 2];
    fill_sine(block, kFrames, 2, 200.0F, 48000);
    const f32 before = rms_of(block, kFrames * 2);
    ChainReport report;
    CY_REQUIRE(chain.process(block, kFrames, 2, report).has_value());
    CY_CHECK_NEAR(rms_of(block, kFrames * 2), before * 0.5F, 1e-4F);

    // A LIMITER: a loud block comes out quieter than it went in.
    EffectChain limiting(allocator(), 48000);
    EffectInstance limiter = effect_of(EffectKind::Limiter, "limiter");
    limiter.target.threshold = 0.25F;
    limiter.target.attack_seconds = 0.0001F;
    limiter.target.gain = 1.0F;
    CY_REQUIRE(limiting.add(limiter).has_value());
    f32 loud[kFrames * 2];
    fill_sine(loud, kFrames, 2, 200.0F, 48000);
    CY_REQUIRE(limiting.process(loud, kFrames, 2, report).has_value());
    CY_CHECK_LT(rms_of(loud, kFrames * 2), before);

    // A GATE: a quiet block comes out silent.
    EffectChain gating(allocator(), 48000);
    EffectInstance gate = effect_of(EffectKind::Gate, "gate");
    gate.target.threshold = 0.5F;
    gate.target.attack_seconds = 0.0001F;
    gate.target.gain = 1.0F;
    CY_REQUIRE(gating.add(gate).has_value());
    f32 quiet[kFrames * 2];
    for (f32& sample : quiet) {
        sample = 0.05F;
    }
    CY_REQUIRE(gating.process(quiet, kFrames, 2, report).has_value());
    CY_CHECK_LT(rms_of(quiet, kFrames * 2), 0.01F);
}

CY_TEST_CASE("audio_effects: a parameter change interpolates rather than jumping") {
    // "Effects SHALL be parameterisable at runtime with interpolated parameter changes."
    EffectChain chain(allocator(), 48000);
    chain.interpolation_seconds = 0.05F;
    EffectInstance gain = effect_of(EffectKind::Gain, "gain");
    gain.target.gain = 1.0F;
    CY_REQUIRE(chain.add(gain).has_value());

    EffectParameters quiet;
    quiet.gain = 0.0F;
    CY_REQUIRE(chain.set_parameters(Name::intern("gain"), quiet).has_value());
    // The target moved; the value in force has not.
    CY_CHECK_EQ(chain.find(Name::intern("gain"))->current.gain, 1.0F);

    chain.advance(1.0F / 60.0F);
    const f32 midway = chain.find(Name::intern("gain"))->current.gain;
    CY_CHECK_LT(midway, 1.0F);
    CY_CHECK_GT(midway, 0.0F);

    for (u32 step = 0; step < 60U; ++step) {
        chain.advance(1.0F / 60.0F);
    }
    CY_CHECK_LT(chain.find(Name::intern("gain"))->current.gain, 0.01F);

    // A parameter set on an effect that is not there is refused rather than ignored.
    CY_CHECK_FALSE(chain.set_parameters(Name::intern("absent"), quiet).has_value());
}

CY_TEST_CASE("audio_effects: latency is reported per effect and summed for the chain") {
    // "SHALL report their latency so the engine can compensate."
    CY_CHECK_EQ(effect_latency_samples(EffectKind::Gain, 48000), 0U);
    CY_CHECK_GT(effect_latency_samples(EffectKind::ReverbConvolution, 48000), 0U);
    CY_CHECK_GT(effect_latency_samples(EffectKind::PitchShift, 48000), 0U);

    EffectChain chain(allocator(), 48000);
    CY_REQUIRE(chain.add(effect_of(EffectKind::Gain, "gain")).has_value());
    CY_REQUIRE(chain.add(effect_of(EffectKind::ReverbConvolution, "reverb")).has_value());
    CY_REQUIRE(chain.add(effect_of(EffectKind::PitchShift, "pitch")).has_value());
    const u32 total = chain.latency_samples();
    CY_CHECK_EQ(total, effect_latency_samples(EffectKind::ReverbConvolution, 48000) +
                           effect_latency_samples(EffectKind::PitchShift, 48000));

    // A bypassed effect adds no latency, because it is not processing.
    chain.find(Name::intern("pitch"))->bypassed = true;
    CY_CHECK_LT(chain.latency_samples(), total);
}

CY_TEST_CASE("audio_effects: an unimplemented effect passes audio through and says so") {
    // The rule this milestone's modules keep: a capability that is not there is QUERYABLE, not
    // silently absent. A chorus that did nothing without saying so would be a bug report from a
    // sound designer.
    CY_CHECK(effect_is_implemented(EffectKind::LowPass));
    CY_CHECK(effect_is_implemented(EffectKind::Delay));
    CY_CHECK_FALSE(effect_is_implemented(EffectKind::Chorus));
    CY_CHECK_FALSE(effect_is_implemented(EffectKind::ReverbAlgorithmic));

    EffectChain chain(allocator(), 48000);
    CY_REQUIRE(chain.add(effect_of(EffectKind::Chorus, "chorus")).has_value());
    CY_CHECK_FALSE(chain.find(Name::intern("chorus"))->implemented);

    constexpr u32 kFrames = 32;
    f32 block[kFrames * 2];
    fill_sine(block, kFrames, 2, 400.0F, 48000);
    const f32 before = rms_of(block, kFrames * 2);
    ChainReport report;
    CY_REQUIRE(chain.process(block, kFrames, 2, report).has_value());
    CY_CHECK_EQ(report.passthrough, 1U);
    CY_CHECK_EQ(report.processed, 0U);
    CY_CHECK_NEAR(rms_of(block, kFrames * 2), before, 1e-6F);
}

CY_TEST_CASE("audio_effects: a delay repeats, and a custom effect is called like a built-in") {
    EffectChain chain(allocator(), 48000);
    EffectInstance delay = effect_of(EffectKind::Delay, "delay");
    delay.target.time_seconds = 0.001F;  // 48 samples
    delay.target.feedback = 0.5F;
    delay.target.mix = 0.5F;
    CY_REQUIRE(chain.add(delay).has_value());

    constexpr u32 kFrames = 256;
    f32 block[kFrames * 2] = {};
    block[0] = 1.0F;  // one impulse in the left channel
    block[1] = 1.0F;
    ChainReport report;
    CY_REQUIRE(chain.process(block, kFrames, 2, report).has_value());
    // The impulse comes back 48 frames later, at half amplitude.
    CY_CHECK_GT(std::fabs(block[static_cast<usize>(48) * 2]), 0.1F);

    // A CUSTOM EFFECT, registered like a built-in and called on the same path.
    EffectChain custom_chain(allocator(), 48000);
    EffectInstance custom = effect_of(EffectKind::Custom, "mute");
    custom.custom = [](f32* samples, u32 frames, u32 channels, const EffectParameters&,
                       void* user) noexcept {
        *static_cast<u32*>(user) += 1U;
        for (u32 index = 0; index < frames * channels; ++index) {
            samples[index] = 0.0F;
        }
    };
    u32 calls = 0;
    custom.custom_user = &calls;
    CY_REQUIRE(custom_chain.add(custom).has_value());
    f32 noisy[64];
    for (f32& sample : noisy) {
        sample = 0.5F;
    }
    CY_REQUIRE(custom_chain.process(noisy, 32, 2, report).has_value());
    CY_CHECK_EQ(calls, 1U);
    CY_CHECK_EQ(noisy[0], 0.0F);
}

CY_TEST_CASE("audio_effects: the analysis tap measures peak, level and bands") {
    EffectChain chain(allocator(), 48000);
    CY_REQUIRE(chain.add(effect_of(EffectKind::AnalysisTap, "meter")).has_value());

    constexpr u32 kFrames = 256;
    f32 block[kFrames * 2];
    fill_sine(block, kFrames, 2, 100.0F, 48000);
    ChainReport report;
    CY_REQUIRE(chain.process(block, kFrames, 2, report).has_value());

    CY_CHECK_GT(chain.analysis().peak, 0.9F);
    CY_CHECK_NEAR(chain.analysis().rms, 0.707F, 0.1F);
    // A low tone puts its energy in the low band rather than the high one.
    CY_CHECK_GT(chain.analysis().bands[0], chain.analysis().bands[3]);
}

CY_TEST_CASE("audio_effects: a panner and a widener behave as their laws say") {
    EffectChain chain(allocator(), 48000);
    EffectInstance panner = effect_of(EffectKind::Panner, "pan");
    panner.target.pan = 1.0F;  // hard right
    CY_REQUIRE(chain.add(panner).has_value());

    constexpr u32 kFrames = 16;
    f32 block[kFrames * 2];
    for (u32 frame = 0; frame < kFrames; ++frame) {
        const usize slot = static_cast<usize>(frame) * 2U;
        block[slot] = 1.0F;
        block[slot + 1] = 1.0F;
    }
    ChainReport report;
    CY_REQUIRE(chain.process(block, kFrames, 2, report).has_value());
    CY_CHECK_LT(std::fabs(block[0]), 0.01F);
    CY_CHECK_NEAR(block[1], 1.0F, 0.01F);

    // A widener at zero makes the two channels the same; above one it pushes them apart.
    EffectChain widening(allocator(), 48000);
    EffectInstance widener = effect_of(EffectKind::StereoWidener, "width");
    widener.target.width = 0.0F;
    CY_REQUIRE(widening.add(widener).has_value());
    f32 stereo[kFrames * 2];
    for (u32 frame = 0; frame < kFrames; ++frame) {
        const usize slot = static_cast<usize>(frame) * 2U;
        stereo[slot] = 1.0F;
        stereo[slot + 1] = -1.0F;
    }
    CY_REQUIRE(widening.process(stereo, kFrames, 2, report).has_value());
    CY_CHECK_NEAR(stereo[0], stereo[1], 1e-5F);
}

CY_TEST_CASE("audio_effects: an empty block and too many channels are refused") {
    EffectChain chain(allocator(), 48000);
    CY_REQUIRE(chain.add(effect_of(EffectKind::Gain, "gain")).has_value());
    ChainReport report;
    f32 block[8] = {};
    CY_CHECK_FALSE(chain.process(nullptr, 4, 2, report).has_value());
    CY_CHECK_FALSE(chain.process(block, 0, 2, report).has_value());
    const Status refused = chain.process(block, 2, 4, report);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::Unsupported);
}
