// The floating-point policy, and the twelve replacements. M9 task 2.3.
//
// EVERY FUNCTION BELOW IS BUILT FROM `sqrt`, `fabs`, `copysign`, ordinary arithmetic, and the
// correctly-rounded set — `exp`, `log`, `atan`. Nothing here calls one of the thirteen the spike
// disqualified, which is checkable by reading the file and is checked by the determinism lint,
// whose one exemption this file is (see lint/determinism_lint.py: the exemption names this path and
// says why).

#include <cy/core/determinism/fp_policy.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>

namespace cy::determinism {
namespace {

// --- The table ----------------------------------------------------------------------------------
//
// `design.md` §1.3's three groups. The order within a group is the spike's own reporting order, and
// the whole table is sorted so that a reader can find a function without searching.

constexpr FloatFunction kPolicy[] = {
    // Exact by IEEE-754. The standard requires correct rounding and this host delivers it.
    {"ceil", FloatClass::ExactByIeee754, false, "IEEE-754 exact"},
    {"copysign", FloatClass::ExactByIeee754, false, "IEEE-754 exact"},
    {"fabs", FloatClass::ExactByIeee754, false, "IEEE-754 exact"},
    {"floor", FloatClass::ExactByIeee754, false, "IEEE-754 exact"},
    {"fma", FloatClass::ExactByIeee754, false, "IEEE-754 exact"},
    {"fmod", FloatClass::ExactByIeee754, false, "IEEE-754 exact"},
    {"nearbyint", FloatClass::ExactByIeee754, false, "IEEE-754 exact"},
    {"remainder", FloatClass::ExactByIeee754, false, "IEEE-754 exact"},
    {"round", FloatClass::ExactByIeee754, false, "IEEE-754 exact"},
    {"sqrt", FloatClass::ExactByIeee754, false, "IEEE-754 exact"},
    {"trunc", FloatClass::ExactByIeee754, false, "IEEE-754 exact"},

    // Correctly rounded in glibc 2.39 on x86-64: zero error against a wider-precision reference on
    // every sample point. A statement about this libm, not about the standard.
    {"atan", FloatClass::CorrectlyRoundedHere, false, "correctly rounded in glibc 2.39"},
    {"cos", FloatClass::CorrectlyRoundedHere, false, "correctly rounded in glibc 2.39"},
    {"erf", FloatClass::CorrectlyRoundedHere, false, "correctly rounded in glibc 2.39"},
    {"exp", FloatClass::CorrectlyRoundedHere, false, "correctly rounded in glibc 2.39"},
    {"exp2", FloatClass::CorrectlyRoundedHere, false, "correctly rounded in glibc 2.39"},
    {"log", FloatClass::CorrectlyRoundedHere, false, "correctly rounded in glibc 2.39"},
    {"log2", FloatClass::CorrectlyRoundedHere, false, "correctly rounded in glibc 2.39"},
    {"pow", FloatClass::CorrectlyRoundedHere, false, "correctly rounded in glibc 2.39"},
    {"sin", FloatClass::CorrectlyRoundedHere, false, "correctly rounded in glibc 2.39"},
    {"tan", FloatClass::CorrectlyRoundedHere, false, "correctly rounded in glibc 2.39"},
    {"tanh", FloatClass::CorrectlyRoundedHere, false, "correctly rounded in glibc 2.39"},

    // The thirteen. Forbidden above `ReplayStable`; twelve have a replacement in `fp`.
    {"acos", FloatClass::NotCorrectlyRounded, true, "folded value differs; 1 sample point wrong"},
    {"acosh", FloatClass::NotCorrectlyRounded, true, "folded value differs; 7 sample points wrong"},
    {"asin", FloatClass::NotCorrectlyRounded, true, "folded value differs; 1 sample point wrong"},
    {"asinh", FloatClass::NotCorrectlyRounded, true, "folded value differs; 3 sample points wrong"},
    {"atan2", FloatClass::NotCorrectlyRounded, true, "folded value differs (two arguments)"},
    {"atanh", FloatClass::NotCorrectlyRounded, true, "folded value differs; 2 sample points wrong"},
    {"cbrt", FloatClass::NotCorrectlyRounded, true, "folded value differs; 12 sample points wrong"},
    {"cosh", FloatClass::NotCorrectlyRounded, true, "folded value differs; 2 sample points wrong"},
    {"expm1", FloatClass::NotCorrectlyRounded, true, "folded value differs; 1 sample point wrong"},
    {"log10", FloatClass::NotCorrectlyRounded, true, "folded value differs; 1 sample point wrong"},
    {"log1p", FloatClass::NotCorrectlyRounded, true, "folded value differs; 1 sample point wrong"},
    {"sinh", FloatClass::NotCorrectlyRounded, true, "folded value differs; 4 sample points wrong"},
    {"tgamma", FloatClass::NotCorrectlyRounded, false,
     "folded value differs; 5 sample points wrong; no replacement shipped, see fp_policy.h"},
};

// The standard's own constants, which are the correctly rounded doubles of these values — this
// file's whole argument is that a value should have one right answer, and writing a literal beside
// a constant the library already defines is two places for it to be wrong. `kHalfPi` is `pi / 2`,
// which is exact in binary because halving only changes the exponent.
constexpr f64 kPi = std::numbers::pi_v<f64>;
constexpr f64 kHalfPi = kPi / 2.0;
constexpr f64 kLn2 = std::numbers::ln2_v<f64>;
constexpr f64 kInvLn10 = std::numbers::log10e_v<f64>;

[[nodiscard]] f64 log1p_kernel(f64 x) noexcept {
    // Kahan's formula. `1 + x` loses the low bits of a small `x`, and dividing by `u - 1` — the
    // rounded increment that `log` actually saw — puts them back. Exact when `u == 1`, where the
    // answer is `x` itself.
    const f64 u = 1.0 + x;
    if (u == 1.0) {
        return x;
    }
    return std::log(u) * x / (u - 1.0);
}

[[nodiscard]] f64 expm1_kernel(f64 x) noexcept {
    const f64 u = std::exp(x);
    if (u == 1.0) {
        return x;
    }
    if (u - 1.0 == -1.0) {
        return -1.0;
    }
    return (u - 1.0) * x / std::log(u);
}

}  // namespace

const char* float_class_name(FloatClass value) noexcept {
    switch (value) {
        case FloatClass::ExactByIeee754:
            return "ExactByIeee754";
        case FloatClass::CorrectlyRoundedHere:
            return "CorrectlyRoundedHere";
        case FloatClass::NotCorrectlyRounded:
            return "NotCorrectlyRounded";
    }
    return "NotCorrectlyRounded";
}

Span<const FloatFunction> float_function_policy() noexcept {
    return {kPolicy, sizeof(kPolicy) / sizeof(kPolicy[0])};
}

const FloatFunction* find_float_function(const char* name) noexcept {
    if (name == nullptr) {
        return nullptr;
    }
    for (const FloatFunction& entry : kPolicy) {
        if (std::strcmp(entry.name, name) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

bool is_forbidden_on_authoritative_path(const char* name) noexcept {
    const FloatFunction* entry = find_float_function(name);
    return entry != nullptr && entry->classification == FloatClass::NotCorrectlyRounded;
}

bool is_permitted_under(const char* name, DeterminismProfile profile) noexcept {
    if (profile == DeterminismProfile::None || profile == DeterminismProfile::ReplayStable) {
        // A replay is replayed by the same binary against the same C library, so a folded constant
        // that is wrong in the same way twice reproduces. Nothing here constrains it.
        return true;
    }
    const FloatFunction* entry = find_float_function(name);
    if (entry == nullptr) {
        // No measurement, so no permission. An unmeasured transcendental is refused rather than
        // waved through: "we did not look" and "we looked and it was fine" must not read alike.
        return false;
    }
    if (entry->classification == FloatClass::ExactByIeee754) {
        return true;
    }
    if (entry->classification == FloatClass::CorrectlyRoundedHere) {
        return profile == DeterminismProfile::SamePlatform;
    }
    return false;
}

u64 ulp_distance(f64 left, f64 right) noexcept {
    if (std::isnan(left) || std::isnan(right)) {
        return ~0ULL;
    }
    if (left == right) {
        return 0;  // Also the +0.0 / -0.0 case, which compare equal and are not a divergence.
    }
    // The standard total order over the bit patterns: a negative double's magnitude bits run the
    // wrong way, so they are reflected around the sign bit before subtracting.
    auto ordered = [](f64 value) noexcept -> i64 {
        const auto bits = std::bit_cast<i64>(value);
        return bits < 0 ? static_cast<i64>(0x8000000000000000ULL) - bits : bits;
    };
    const i64 a = ordered(left);
    const i64 b = ordered(right);
    return a >= b ? static_cast<u64>(a - b) : static_cast<u64>(b - a);
}

u64 ulp_distance(f32 left, f32 right) noexcept {
    if (std::isnan(left) || std::isnan(right)) {
        return ~0ULL;
    }
    if (left == right) {
        return 0;
    }
    auto ordered = [](f32 value) noexcept -> i32 {
        const auto bits = std::bit_cast<i32>(value);
        return bits < 0 ? (static_cast<i32>(0x80000000U) - bits) : bits;
    };
    const i64 a = ordered(left);
    const i64 b = ordered(right);
    return a >= b ? static_cast<u64>(a - b) : static_cast<u64>(b - a);
}

namespace fp {

f64 acos(f64 x) noexcept {
    // acos(x) = 2 atan(sqrt((1 - x) / (1 + x))). At x = -1 the quotient is infinite and atan
    // saturates at pi/2, which is the right answer; the endpoints are still handled explicitly so
    // that the result is exactly 0 and exactly pi rather than within a rounding of them.
    if (x >= 1.0) {
        return 0.0;
    }
    if (x <= -1.0) {
        return kPi;
    }
    return 2.0 * std::atan(std::sqrt((1.0 - x) / (1.0 + x)));
}

f64 asin(f64 x) noexcept {
    if (x >= 1.0) {
        return kHalfPi;
    }
    if (x <= -1.0) {
        return -kHalfPi;
    }
    // (1 - x)(1 + x) rather than 1 - x*x: the factored form loses no bits when |x| is near one,
    // which is where every asin implementation is at its worst.
    return std::atan(x / std::sqrt((1.0 - x) * (1.0 + x)));
}

f64 atan2(f64 y, f64 x) noexcept {
    if (x > 0.0) {
        return std::atan(y / x);
    }
    if (x < 0.0) {
        // The branch cut is on the negative real axis, and which side of it a point sits on is
        // decided by the *sign* of y rather than by its value — so atan2(-0.0, -1) is -pi and
        // atan2(+0.0, -1) is +pi, which `copysign` gets right and a comparison against zero does
        // not.
        return std::atan(y / x) + std::copysign(kPi, y);
    }
    if (y != 0.0) {
        return std::copysign(kHalfPi, y);
    }
    return std::copysign(0.0, y);
}

f64 acosh(f64 x) noexcept {
    if (x < 1.0) {
        return std::numeric_limits<f64>::quiet_NaN();
    }
    if (x > 1.0e8) {
        // x*x would be the whole of the answer and would overflow above 1e154. acosh(x) -> log(2x)
        // long before that, to well inside a rounding.
        return std::log(x) + kLn2;
    }
    const f64 t = x - 1.0;
    // log1p of (t + sqrt(2t + t*t)) rather than log of (x + sqrt(x*x - 1)): near x = 1 the second
    // form computes log of something within a rounding of 1 and returns noise.
    return log1p_kernel(t + std::sqrt((2.0 * t) + (t * t)));
}

f64 asinh(f64 x) noexcept {
    const f64 a = std::fabs(x);
    f64 result = 0.0;
    if (a > 1.0e8) {
        result = std::log(a) + kLn2;
    } else {
        // a + a*a / (1 + sqrt(1 + a*a)) is (sqrt(a*a + 1) - 1) + a, rearranged so that the
        // subtraction never happens. The argument to log1p is then accurate down to the smallest
        // a, where it is a itself.
        result = log1p_kernel(a + ((a * a) / (1.0 + std::sqrt(1.0 + (a * a)))));
    }
    return std::copysign(result, x);
}

f64 atanh(f64 x) noexcept {
    if (x == 1.0) {
        return std::numeric_limits<f64>::infinity();
    }
    if (x == -1.0) {
        return -std::numeric_limits<f64>::infinity();
    }
    if (x > 1.0 || x < -1.0) {
        return std::numeric_limits<f64>::quiet_NaN();
    }
    // 0.5 (log1p(x) - log1p(-x)) rather than the textbook 0.5 log1p(2x / (1 - x)).
    //
    // MEASURED, not preferred: the textbook form was 37 618 ulp wrong at x = -0.999999, because
    // `2x / (1 - x)` rounds to a value near -1 and log1p's derivative there is 2e6 — a last-bit
    // error in the argument becomes a ten-thousand-ulp error in the answer. This form feeds each
    // log1p an argument it is accurate at: near x = 1, `1 + (-x)` is exact by Sterbenz, so
    // log1p(-x) is log of a well-conditioned small number. For small x the two logs are +x and -x
    // and the subtraction is an addition, so there is no cancellation either.
    return 0.5 * (log1p_kernel(x) - log1p_kernel(-x));
}

f64 cbrt(f64 x) noexcept {
    if (x == 0.0 || !std::isfinite(x)) {
        return x;
    }
    const f64 a = std::fabs(x);
    // exp(log(a)/3) is a good seed and a poor answer: it carries three roundings. Two Newton steps
    // on y**3 = a take it to the last bit or two, using only division and subtraction.
    f64 y = std::exp(std::log(a) / 3.0);
    y -= (y - (a / (y * y))) / 3.0;
    y -= (y - (a / (y * y))) / 3.0;
    return std::copysign(y, x);
}

f64 cosh(f64 x) noexcept {
    const f64 a = std::fabs(x);
    if (a > 710.5) {
        return std::numeric_limits<f64>::infinity();
    }
    if (a > 709.0) {
        // The overflow tail only. h = exp(a)/2 computed as exp(a - ln 2), so the halving happens in
        // the exponent and cosh does not overflow a whole order of magnitude before it has to; then
        // cosh(a) = h + 1/(4h). It costs accuracy — subtracting ln 2 from a large argument rounds,
        // and exp amplifies that by the argument's magnitude, which measured 14 ulp at a = 20 — so
        // it is used ONLY where `exp(a)` itself would be infinite and there is no alternative.
        const f64 h = std::exp(a - kLn2);
        return h + (0.25 / h);
    }
    const f64 e = std::exp(a);
    // Halving by 0.5 is exact, and 1/e is a rounding of a number that is smaller than e for every
    // a > 0, so the sum carries about one rounding.
    return 0.5 * (e + (1.0 / e));
}

f64 sinh(f64 x) noexcept {
    const f64 a = std::fabs(x);
    if (a > 710.5) {
        return std::copysign(std::numeric_limits<f64>::infinity(), x);
    }
    f64 result = 0.0;
    if (a > 709.0) {
        // The overflow tail. See cosh() for why this form is confined to it.
        const f64 h = std::exp(a - kLn2);
        result = h - (0.25 / h);
    } else if (a > 1.0) {
        const f64 e = std::exp(a);
        result = 0.5 * (e - (1.0 / e));
    } else {
        // Below 1 the two exponentials agree to several digits and subtracting them throws those
        // digits away. expm1 does not: with t = e**a - 1, sinh(a) = t(t + 2) / (2(t + 1)), and for
        // small a that is a itself.
        const f64 t = expm1_kernel(a);
        result = (0.5 * t * (t + 2.0)) / (t + 1.0);
    }
    return std::copysign(result, x);
}

f64 expm1(f64 x) noexcept {
    if (x > 709.8) {
        return std::numeric_limits<f64>::infinity();
    }
    return expm1_kernel(x);
}

f64 log1p(f64 x) noexcept {
    if (x < -1.0) {
        return std::numeric_limits<f64>::quiet_NaN();
    }
    if (x == -1.0) {
        return -std::numeric_limits<f64>::infinity();
    }
    return log1p_kernel(x);
}

f64 log10(f64 x) noexcept {
    // One correctly-rounded log and one multiplication, so two roundings rather than glibc's own
    // answer. Within 2 ulp, and — the property this file exists for — the same 2 ulp whether the
    // compiler folds it or leaves it to run time.
    return std::log(x) * kInvLn10;
}

// The `float` overloads compute in `double` and round once at the end. That is both more accurate
// than evaluating in `float` and, more to the point here, one rounding rather than a chain of them.
f32 acos(f32 x) noexcept {
    return static_cast<f32>(acos(static_cast<f64>(x)));
}
f32 asin(f32 x) noexcept {
    return static_cast<f32>(asin(static_cast<f64>(x)));
}
f32 atan2(f32 y, f32 x) noexcept {
    return static_cast<f32>(atan2(static_cast<f64>(y), static_cast<f64>(x)));
}
f32 acosh(f32 x) noexcept {
    return static_cast<f32>(acosh(static_cast<f64>(x)));
}
f32 asinh(f32 x) noexcept {
    return static_cast<f32>(asinh(static_cast<f64>(x)));
}
f32 atanh(f32 x) noexcept {
    return static_cast<f32>(atanh(static_cast<f64>(x)));
}
f32 cbrt(f32 x) noexcept {
    return static_cast<f32>(cbrt(static_cast<f64>(x)));
}
f32 cosh(f32 x) noexcept {
    return static_cast<f32>(cosh(static_cast<f64>(x)));
}
f32 sinh(f32 x) noexcept {
    return static_cast<f32>(sinh(static_cast<f64>(x)));
}
f32 expm1(f32 x) noexcept {
    return static_cast<f32>(expm1(static_cast<f64>(x)));
}
f32 log1p(f32 x) noexcept {
    return static_cast<f32>(log1p(static_cast<f64>(x)));
}
f32 log10(f32 x) noexcept {
    return static_cast<f32>(log10(static_cast<f64>(x)));
}

}  // namespace fp
}  // namespace cy::determinism
