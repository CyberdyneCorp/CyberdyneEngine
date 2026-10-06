// SPDX-License-Identifier: MIT
// The kernel digest. Design §10.1 and §10.2; the definition is digest.h's, and
// tools/detmath/model.py states the same sweep in Python. The two must change together.

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

Fixed draw_fixed(SplitMix64& rng) noexcept {
    return Fixed::from_raw(rng.scaled());
}

u64 arithmetic_step(KernelFunction function, SplitMix64& rng) noexcept {
    switch (function) {
        case KernelFunction::Add: {
            const auto a = static_cast<i64>(rng.next());
            const auto b = static_cast<i64>(rng.next());
            return bits(Fixed::from_raw(a) + Fixed::from_raw(b));
        }
        case KernelFunction::Subtract: {
            const auto a = static_cast<i64>(rng.next());
            const auto b = static_cast<i64>(rng.next());
            return bits(Fixed::from_raw(a) - Fixed::from_raw(b));
        }
        case KernelFunction::Multiply: {
            const Fixed a = draw_fixed(rng);
            return bits(a * draw_fixed(rng));
        }
        case KernelFunction::Divide: {
            const Fixed a = draw_fixed(rng);
            return bits(a / draw_fixed(rng));
        }
        case KernelFunction::Sqrt:
            return bits(sqrt(draw_fixed(rng)));
        case KernelFunction::SqrtWide: {
            const i64 high = rng.scaled();
            const u64 low = rng.next();
            return bits(sqrt(WideFixed::from_raw(U128{low, static_cast<u64>(high)})));
        }
        case KernelFunction::NarrowFixed16:
            return static_cast<u64>(static_cast<i64>(Fixed16::narrow(draw_fixed(rng)).raw));
        case KernelFunction::AngleScale: {
            const Angle a = Angle::from_raw(rng.angle());
            return bits(a * Fixed::from_raw(rng.scaled() >> 16));
        }
        case KernelFunction::AngleFromRadians:
            return bits(Angle::from_radians(draw_fixed(rng)));
        case KernelFunction::AngleRadians:
            return bits(Angle::from_raw(rng.angle()).radians());
        default:
            return 0;
    }
}

u64 transcendental_step(KernelFunction function, SplitMix64& rng) noexcept {
    switch (function) {
        case KernelFunction::Sin:
            return bits(sin(Angle::from_raw(rng.angle())));
        case KernelFunction::Cos:
            return bits(cos(Angle::from_raw(rng.angle())));
        case KernelFunction::Tan:
            return bits(tan(Angle::from_raw(rng.angle())));
        case KernelFunction::Atan:
            return bits(atan(draw_fixed(rng)));
        case KernelFunction::Atan2: {
            const Fixed y = draw_fixed(rng);
            return bits(atan2(y, draw_fixed(rng)));
        }
        case KernelFunction::Asin:
            return bits(asin(Fixed::from_raw(rng.ranged(-5 * (kOne >> 2), 5 * (kOne >> 2)))));
        case KernelFunction::Acos:
            return bits(acos(Fixed::from_raw(rng.ranged(-5 * (kOne >> 2), 5 * (kOne >> 2)))));
        case KernelFunction::Exp2:
            return bits(exp2(Fixed::from_raw(rng.ranged(-40 * kOne, 40 * kOne))));
        case KernelFunction::Log2:
            return bits(log2(Fixed::from_raw(rng.positive())));
        case KernelFunction::Exp:
            return bits(exp(Fixed::from_raw(rng.ranged(-40 * kOne, 40 * kOne))));
        case KernelFunction::Log:
            return bits(log(Fixed::from_raw(rng.positive())));
        case KernelFunction::Pow: {
            const Fixed x = Fixed::from_raw(rng.positive());
            return bits(pow(x, Fixed::from_raw(rng.ranged(-8 * kOne, 8 * kOne))));
        }
        default:
            return arithmetic_step(function, rng);
    }
}

u64 function_seed(KernelFunction function) noexcept {
    return kSweepSeed + (0x1000ULL * (static_cast<u64>(function) + 1));
}

}  // namespace

const char* kernel_function_name(KernelFunction function) noexcept {
    constexpr const char* kNames[] = {
        "add",           "sub",         "mul",
        "div",           "sqrt",        "sqrt_wide",
        "narrow16",      "angle_scale", "angle_from_radians",
        "angle_radians", "sin",         "cos",
        "tan",           "atan",        "atan2",
        "asin",          "acos",        "exp2",
        "log2",          "exp",         "log",
        "pow",
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
    for (u32 index = 0; index < count; ++index) {
        accumulator = digest_fold(accumulator, transcendental_step(function, rng));
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
