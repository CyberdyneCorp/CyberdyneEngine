// SPDX-License-Identifier: MIT
// The level's sounds. See sounds.h.

#include "sounds.h"

#include <cmath>

namespace sample::tps {
namespace {

using cy::f32;
using cy::u32;

constexpr f32 kTau = 6.28318531F;

/// A fixed linear congruential sequence of values in [-1, 1).
class Noise {
public:
    explicit Noise(u32 seed) noexcept : state_(seed) {}
    f32 next() noexcept {
        state_ = (state_ * 1664525U) + 1013904223U;
        return (static_cast<f32>(state_ >> 8U) / 8388608.0F) - 1.0F;
    }

private:
    u32 state_;
};

/// A one-pole low-pass: `amount` near one is dark, near zero is bright.
class LowPass {
public:
    explicit LowPass(f32 amount) noexcept : amount_(amount) {}
    f32 operator()(f32 input) noexcept {
        value_ = (value_ * amount_) + (input * (1.0F - amount_));
        return value_;
    }

private:
    f32 amount_;
    f32 value_ = 0.0F;
};

[[nodiscard]] std::vector<f32> silence(u32 sample_rate, f32 seconds) {
    return std::vector<f32>(static_cast<std::size_t>(static_cast<f32>(sample_rate) * seconds),
                            0.0F);
}

/// A boot on stone: a bright scuff over a short low knock.
[[nodiscard]] std::vector<f32> step_stone(u32 rate) {
    std::vector<f32> out = silence(rate, 0.12F);
    Noise noise(0x5701E001U);
    LowPass dark(0.35F);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(rate);
        const f32 scuff = (noise.next() - dark(noise.next())) * std::exp(-t * 70.0F);
        const f32 knock = std::sin(kTau * 140.0F * t) * std::exp(-t * 45.0F);
        out[i] = (0.45F * scuff) + (0.55F * knock);
    }
    return out;
}

/// A boot in grass: a soft, longer brush of noise.
[[nodiscard]] std::vector<f32> step_grass(u32 rate) {
    std::vector<f32> out = silence(rate, 0.18F);
    Noise noise(0x67A55002U);
    LowPass soft(0.6F);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(rate);
        const f32 envelope = std::fmin(t * 120.0F, 1.0F) * std::exp(-t * 22.0F);
        out[i] = 0.5F * soft(noise.next()) * envelope;
    }
    return out;
}

/// A jump: a breath of air that swells and falls away.
[[nodiscard]] std::vector<f32> jump(u32 rate) {
    std::vector<f32> out = silence(rate, 0.3F);
    Noise noise(0x10A0F003U);
    LowPass air(0.85F);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(rate);
        const f32 envelope = std::sin(3.14159265F * std::fmin(t / 0.3F, 1.0F));
        out[i] = 1.4F * air(noise.next()) * envelope * envelope;
    }
    return out;
}

/// A landing: a low thump and a scatter of grit.
[[nodiscard]] std::vector<f32> land(u32 rate) {
    std::vector<f32> out = silence(rate, 0.25F);
    Noise noise(0x1A4D0004U);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(rate);
        const f32 thump = std::sin(kTau * 72.0F * t) * std::exp(-t * 18.0F);
        const f32 grit = noise.next() * std::exp(-t * 40.0F);
        out[i] = (0.75F * thump) + (0.2F * grit);
    }
    return out;
}

/// The wind: dark noise breathing over six seconds, which loop seamlessly — the swell is periodic
/// in the loop, and the filter is run over the loop twice so its end leads into its start.
[[nodiscard]] std::vector<f32> wind(u32 rate) {
    std::vector<f32> out = silence(rate, 6.0F);
    Noise noise(0x0F1D0005U);
    std::vector<f32> raw(out.size());
    for (f32& sample : raw) {
        sample = noise.next();
    }
    LowPass dark(0.985F);
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = dark(raw[i]);
        }
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        const f32 phase = static_cast<f32>(i) / static_cast<f32>(out.size());
        out[i] *= 4.0F * (0.65F + (0.35F * std::sin(kTau * phase)));
    }
    return out;
}

/// A bird: three rising chirps.
[[nodiscard]] std::vector<f32> bird(u32 rate) {
    std::vector<f32> out = silence(rate, 0.5F);
    f32 phase = 0.0F;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(rate);
        const f32 within = std::fmod(t, 0.16F);
        const bool chirping = within < 0.1F && t < 0.48F;
        const f32 frequency = 2800.0F + (14000.0F * within);
        phase += kTau * frequency / static_cast<f32>(rate);
        const f32 envelope = chirping ? std::sin(3.14159265F * within / 0.1F) : 0.0F;
        out[i] = 0.3F * std::sin(phase) * envelope;
    }
    return out;
}

}  // namespace

std::vector<Sound> synthesise_sounds(u32 sample_rate) {
    std::vector<Sound> sounds;
    sounds.push_back(Sound{"step.stone", step_stone(sample_rate), false});
    sounds.push_back(Sound{"step.grass", step_grass(sample_rate), false});
    sounds.push_back(Sound{"hero.jump", jump(sample_rate), false});
    sounds.push_back(Sound{"hero.land", land(sample_rate), false});
    sounds.push_back(Sound{"ambience.wind", wind(sample_rate), true});
    sounds.push_back(Sound{"ambience.bird", bird(sample_rate), false});
    return sounds;
}

}  // namespace sample::tps
