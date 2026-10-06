// SPDX-License-Identifier: MIT
// The kernel digest. Design §10.1 and §10.2; the definition is digest.h's, and
// tools/detmath/model.py states the same sweep in Python. The two must change together.

#include "kernel.h"

#include <cy/core/detmath/digest.h>
#include <cy/core/detmath/fixed.h>
#include <cy/core/detmath/functions.h>
#include <cy/core/detmath/version.h>

namespace cy::detmath::inline CY_DETMATH_VARIANT {
namespace {

constexpr u64 kGolden = 0x9E3779B97F4A7C15ULL;

constexpr u64 finalize(u64 z) noexcept {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/// SplitMix64. Chosen for the sweep because it is three lines in every language a kernel will be
/// written in, so a second implementation can reproduce the inputs without a library.
class SplitMix64 {
public:
    explicit SplitMix64(u64 seed) noexcept : state_(seed) {}

    u64 next() noexcept {
        state_ += kGolden;
        return finalize(state_);
    }

    /// A raw value spread over every binade: a random i64 shifted right by a random amount.
    i64 scaled() noexcept {
        const auto value = static_cast<i64>(next());
        const auto shift = static_cast<int>(next() & 63U);
        return value >> shift;
    }

    /// A raw value in [low, high).
    i64 ranged(i64 low, i64 high) noexcept {
        const u64 span = static_cast<u64>(high) - static_cast<u64>(low);
        return low + static_cast<i64>(next() % span);
    }

    /// A positive raw value.
    i64 positive() noexcept {
        const i64 value = scaled() & INT64_MAX;
        return value != 0 ? value : 1;
    }

    u32 angle() noexcept { return static_cast<u32>(next() >> 32); }

private:
    u64 state_;
};

constexpr i64 kOne = Fixed::kOneRaw;

u64 bits(Fixed value) noexcept {
    return static_cast<u64>(value.raw);
}

u64 bits(Angle value) noexcept {
    return value.raw;
}

u64 bits(i64 value) noexcept {
    return static_cast<u64>(value);
}

Fixed fx(u64 raw) noexcept {
    return Fixed::from_raw(static_cast<i64>(raw));
}

Angle an(u64 raw) noexcept {
    return Angle::from_raw(static_cast<u32>(raw));
}

/// The inputs of one sweep step. The order of the draws is part of the digest's definition;
/// tools/detmath/model.py's `draw()` states the same order.
void draw_arithmetic(KernelFunction function, SplitMix64& rng, u64* inputs) noexcept {
    switch (function) {
        case KernelFunction::Add:
        case KernelFunction::Subtract:
            inputs[0] = rng.next();
            inputs[1] = rng.next();
            return;
        case KernelFunction::Multiply:
        case KernelFunction::Divide:
            inputs[0] = bits(rng.scaled());
            inputs[1] = bits(rng.scaled());
            return;
        case KernelFunction::SqrtWide:
            inputs[0] = bits(rng.scaled());  // the high limb
            inputs[1] = rng.next();          // the low limb
            return;
        case KernelFunction::AngleScale:
            inputs[0] = rng.angle();
            inputs[1] = bits(rng.scaled() >> 16);
            return;
        case KernelFunction::AngleRadians:
            inputs[0] = rng.angle();
            return;
        default:  // Sqrt, NarrowFixed16, AngleFromRadians
            inputs[0] = bits(rng.scaled());
            return;
    }
}

void draw(KernelFunction function, SplitMix64& rng, u64* inputs) noexcept {
    switch (function) {
        case KernelFunction::Sin:
        case KernelFunction::Cos:
        case KernelFunction::Tan:
        case KernelFunction::SinCore:
        case KernelFunction::CosCore:
            inputs[0] = rng.angle();
            return;
        case KernelFunction::Atan:
            inputs[0] = bits(rng.scaled());
            return;
        case KernelFunction::Atan2:
            inputs[0] = bits(rng.scaled());
            inputs[1] = bits(rng.scaled());
            return;
        case KernelFunction::Asin:
        case KernelFunction::Acos:
            inputs[0] = bits(rng.ranged(-5 * (kOne >> 2), 5 * (kOne >> 2)));
            return;
        case KernelFunction::Exp2:
        case KernelFunction::Exp:
            inputs[0] = bits(rng.ranged(-40 * kOne, 40 * kOne));
            return;
        case KernelFunction::Log2:
        case KernelFunction::Log:
        case KernelFunction::Log2Core:
            inputs[0] = bits(rng.positive());
            return;
        case KernelFunction::Pow:
            inputs[0] = bits(rng.positive());
            inputs[1] = bits(rng.ranged(-8 * kOne, 8 * kOne));
            return;
        case KernelFunction::AtanCore:
        case KernelFunction::Exp2Core:
            inputs[0] = rng.next() >> 2;  // a Q2.62 value in [0, 1)
            return;
        default:
            draw_arithmetic(function, rng, inputs);
            return;
    }
}

u64 evaluate_arithmetic(KernelFunction function, const u64* in) noexcept {
    switch (function) {
        case KernelFunction::Add:
            return bits(fx(in[0]) + fx(in[1]));
        case KernelFunction::Subtract:
            return bits(fx(in[0]) - fx(in[1]));
        case KernelFunction::Multiply:
            return bits(fx(in[0]) * fx(in[1]));
        case KernelFunction::Divide:
            return bits(fx(in[0]) / fx(in[1]));
        case KernelFunction::Sqrt:
            return bits(sqrt(fx(in[0])));
        case KernelFunction::SqrtWide:
            return bits(sqrt(WideFixed::from_raw(U128{in[1], in[0]})));
        case KernelFunction::NarrowFixed16:
            return bits(i64{Fixed16::narrow(fx(in[0])).raw});
        case KernelFunction::AngleScale:
            return bits(an(in[0]) * fx(in[1]));
        case KernelFunction::AngleFromRadians:
            return bits(Angle::from_radians(fx(in[0])));
        case KernelFunction::AngleRadians:
            return bits(an(in[0]).radians());
        default:
            return 0;
    }
}

u64 evaluate_core(KernelFunction function, const u64* in) noexcept {
    switch (function) {
        case KernelFunction::SinCore:
            return bits(kernel::sincos62(static_cast<u32>(in[0])).sin);
        case KernelFunction::CosCore:
            return bits(kernel::sincos62(static_cast<u32>(in[0])).cos);
        case KernelFunction::AtanCore:
            return bits(kernel::atan_turns62(static_cast<i64>(in[0])));
        case KernelFunction::Exp2Core:
            return bits(kernel::exp2_value62(in[0]));
        case KernelFunction::Log2Core: {
            const auto x = static_cast<i64>(in[0]);
            return x > 0 ? bits(kernel::log2_parts(x).remainder62) : 0;
        }
        default:
            return evaluate_arithmetic(function, in);
    }
}

u64 function_seed(KernelFunction function) noexcept {
    return kSweepSeed + (0x1000ULL * (static_cast<u64>(function) + 1));
}

}  // namespace

u64 evaluate_raw(KernelFunction function, const u64* in) noexcept {
    switch (function) {
        case KernelFunction::Sin:
            return bits(sin(an(in[0])));
        case KernelFunction::Cos:
            return bits(cos(an(in[0])));
        case KernelFunction::Tan:
            return bits(tan(an(in[0])));
        case KernelFunction::Atan:
            return bits(atan(fx(in[0])));
        case KernelFunction::Atan2:
            return bits(atan2(fx(in[0]), fx(in[1])));
        case KernelFunction::Asin:
            return bits(asin(fx(in[0])));
        case KernelFunction::Acos:
            return bits(acos(fx(in[0])));
        case KernelFunction::Exp2:
            return bits(exp2(fx(in[0])));
        case KernelFunction::Log2:
            return bits(log2(fx(in[0])));
        case KernelFunction::Exp:
            return bits(exp(fx(in[0])));
        case KernelFunction::Log:
            return bits(log(fx(in[0])));
        case KernelFunction::Pow:
            return bits(pow(fx(in[0]), fx(in[1])));
        default:
            return evaluate_core(function, in);
    }
}

const char* kernel_function_name(KernelFunction function) noexcept {
    constexpr const char* kNames[] = {
        "add",           "sub",         "mul",
        "div",           "sqrt",        "sqrt_wide",
        "narrow16",      "angle_scale", "angle_from_radians",
        "angle_radians", "sin",         "cos",
        "tan",           "atan",        "atan2",
        "asin",          "acos",        "exp2",
        "log2",          "exp",         "log",
        "pow",           "sin_core",    "cos_core",
        "atan_core",     "exp2_core",   "log2_core",
    };
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<u64>(KernelFunction::Count));
    const auto index = static_cast<u64>(function);
    return index < static_cast<u64>(KernelFunction::Count) ? kNames[index] : "";
}

u64 digest_fold(u64 accumulator, u64 value) noexcept {
    return finalize((accumulator ^ value) + kGolden);
}

u64 function_digest(KernelFunction function, u32 count) noexcept {
    SplitMix64 rng(function_seed(function));
    u64 accumulator = function_seed(function);
    u64 inputs[2] = {0, 0};
    for (u32 index = 0; index < count; ++index) {
        draw(function, rng, inputs);
        accumulator = digest_fold(accumulator, evaluate_raw(function, inputs));
    }
    return accumulator;
}

u64 kernel_digest() noexcept {
    u64 accumulator = digest_fold(kSweepSeed, kKernelVersion);
    for (u8 index = 0; index < static_cast<u8>(KernelFunction::Count); ++index) {
        accumulator = digest_fold(accumulator, function_digest(static_cast<KernelFunction>(index)));
    }
    return accumulator;
}

}  // namespace cy::detmath::inline CY_DETMATH_VARIANT
