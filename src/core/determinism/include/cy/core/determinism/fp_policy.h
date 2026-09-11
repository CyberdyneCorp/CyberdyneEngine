#pragma once
// The floating-point policy, and the thirteen functions the spike disqualified. M9 task 2.3.
//
// ================================================================================================
// THIS FILE IS A MEASUREMENT, NOT AN OPINION
// ================================================================================================
//
// `design.md` §1.3 records what M9's spike measured over thirty-eight `<cmath>` functions in
// `float` and `double`, across twenty builds — clang 18 and GCC 13, at `-O0`, `-O1`, `-O2` and
// `-O3 -flto`, with and without `-ffp-contract=off`, plus `-march=native`. Three groups came out of
// it and this header is that table, so that no consumer has to re-derive it:
//
//   EXACT BY IEEE-754, and measured exact here.
//       sqrt fabs floor ceil trunc round nearbyint fma fmod remainder copysign
//     The standard requires these to be correctly rounded. Authoritative code may use them under
//     every profile.
//
//   CORRECTLY ROUNDED IN **THIS** LIBM, scored zero against a wider-precision reference on every
//   sample point.
//       exp exp2 log log2 sin cos tan atan tanh erf pow
//     That is a statement about glibc 2.39 on this host and NOT about the standard, so these are
//     permitted under `SamePlatform` — where the binary and the C library are the same by
//     definition — and refused under `CrossPlatform` and `Lockstep`, which are claims about
//     machines this project has never compared.
//
//   NOT CORRECTLY ROUNDED HERE, and the compiler changes their value when it folds them.
//       acos acosh asin asinh atan2 atanh cbrt cosh expm1 log10 log1p sinh tgamma
//     Thirteen functions, and the two halves of the spike corroborate each other point for point:
//     the functions whose *folded* value moves are, count for count, the functions this libm does
//     not round correctly. A function that is not correctly rounded has no unique right answer, so
//     its value is a property of this glibc build rather than of IEEE-754 — which is exactly the
//     value a different distribution is free to change. **They are forbidden to an authoritative
//     path under every profile above `ReplayStable`**, and the engine ships its own.
//
// Why `ReplayStable` is the exception: a replay is replayed by the same binary against the same C
// library, so a folded constant that is wrong in the same way twice reproduces. The moment two
// binaries have to agree — `SamePlatform` already, because `-O0` and `-O2` of the same source
// disagree about a folded `acos` — it stops reproducing.
//
// ================================================================================================
// WHAT `cy::determinism::fp` IS, AND WHAT IT IS NOT
// ================================================================================================
//
// Each replacement below is built **only** out of the exact set and the correctly-rounded set. That
// buys one specific property and it is worth being precise about it: whether the compiler evaluates
// the expression itself or leaves it to run time, it is evaluating the same operations, and every
// one of those operations has a unique right answer on this platform. So `fp::acos(0.3)` has the
// same value at `-O0` and at `-O3`, which `std::acos(0.3)` measurably does not.
//
// **It is not a cross-platform claim and must not be read as one.** The correctly-rounded set is
// correctly rounded *in glibc 2.39 on x86-64*; a different libm may round `exp` differently and
// every function here would move with it. `CrossPlatform` and `Lockstep` need deterministic math
// types — fixed-point scalars and polynomial approximations that compute their own answers — and
// this tree has none, which is why `DeterminismConfiguration::require()` refuses those two profiles
// outright (profile.h). This file is the `SamePlatform` answer and says so.

#include <cy/core/base/types.h>
#include <cy/core/determinism/profile.h>
#include <cy/core/memory/array.h>

namespace cy::determinism {

/// What the spike measured about one function.
enum class FloatClass : u8 {
    /// IEEE-754 requires a correctly rounded result, and this host delivers one.
    ExactByIeee754 = 0,
    /// Correctly rounded by **this** libm on every sample point. Not a property of the standard.
    CorrectlyRoundedHere,
    /// Not correctly rounded here, and its folded value differs from its runtime value.
    NotCorrectlyRounded,
};

const char* float_class_name(FloatClass value) noexcept;

/// One row of the table above.
struct FloatFunction {
    /// The `<cmath>` spelling, without a namespace. What the lint greps for.
    const char* name = "";
    FloatClass classification = FloatClass::NotCorrectlyRounded;
    /// True when `cy::determinism::fp` ships a replacement an authoritative path may call instead.
    ///
    /// False for `tgamma`, and that is a deliberate omission rather than an oversight: it is on the
    /// forbidden list because it was measured wrong, not because a simulation needs it. Shipping a
    /// Lanczos approximation nobody calls would be code with no reader and no caller to keep it
    /// honest. A project that needs one writes it and declares it.
    bool replacement_available = false;
    /// The measurement, in a phrase. Never null.
    const char* note = "";
};

/// The whole table, in the order the spike reports it. Read by the determinism lint, by the
/// documentation generator, and by anything asking whether a call is legal.
[[nodiscard]] Span<const FloatFunction> float_function_policy() noexcept;

/// The row for `name`, or null when the function is not one the spike measured. Null means "no
/// opinion" and callers must not read it as "permitted" — the lint reports an unmeasured
/// transcendental separately.
[[nodiscard]] const FloatFunction* find_float_function(const char* name) noexcept;

/// True for the thirteen. The predicate the lint's forbidden-call rule is written against.
[[nodiscard]] bool is_forbidden_on_authoritative_path(const char* name) noexcept;

/// May authoritative code under `profile` call `name`?
///
/// `None` and `ReplayStable` permit everything — see the header comment for why. `SamePlatform`
/// permits the exact and the correctly-rounded sets. `CrossPlatform` and `Lockstep` permit the
/// exact set alone, because everything else is a property of one C library.
[[nodiscard]] bool is_permitted_under(const char* name, DeterminismProfile profile) noexcept;

/// Distance in units in the last place, saturating at `~0ULL` for a sign or class mismatch.
///
/// Here rather than in a test because two comparisons need it — the replacements' accuracy, and the
/// validator's report of *how far apart* two runs' values were, which is the difference between
/// "one rounding" and "a different algorithm".
[[nodiscard]] u64 ulp_distance(f64 left, f64 right) noexcept;
[[nodiscard]] u64 ulp_distance(f32 left, f32 right) noexcept;

// --- The replacements ---------------------------------------------------------------------------
//
// Twelve of the thirteen. Every one is built from the exact set (`sqrt`, `fabs`, `copysign`,
// arithmetic) and the correctly-rounded set (`exp`, `log`, `atan`, `sin`), and from nothing else.
// The `float` overloads compute in `double` and round once, which is both more accurate than a
// `float` evaluation and — more to the point here — one rounding rather than a chain of them.

namespace fp {

[[nodiscard]] f64 acos(f64 x) noexcept;
[[nodiscard]] f64 asin(f64 x) noexcept;
[[nodiscard]] f64 atan2(f64 y, f64 x) noexcept;
[[nodiscard]] f64 acosh(f64 x) noexcept;
[[nodiscard]] f64 asinh(f64 x) noexcept;
[[nodiscard]] f64 atanh(f64 x) noexcept;
[[nodiscard]] f64 cbrt(f64 x) noexcept;
[[nodiscard]] f64 cosh(f64 x) noexcept;
[[nodiscard]] f64 sinh(f64 x) noexcept;
[[nodiscard]] f64 expm1(f64 x) noexcept;
[[nodiscard]] f64 log1p(f64 x) noexcept;
[[nodiscard]] f64 log10(f64 x) noexcept;

[[nodiscard]] f32 acos(f32 x) noexcept;
[[nodiscard]] f32 asin(f32 x) noexcept;
[[nodiscard]] f32 atan2(f32 y, f32 x) noexcept;
[[nodiscard]] f32 acosh(f32 x) noexcept;
[[nodiscard]] f32 asinh(f32 x) noexcept;
[[nodiscard]] f32 atanh(f32 x) noexcept;
[[nodiscard]] f32 cbrt(f32 x) noexcept;
[[nodiscard]] f32 cosh(f32 x) noexcept;
[[nodiscard]] f32 sinh(f32 x) noexcept;
[[nodiscard]] f32 expm1(f32 x) noexcept;
[[nodiscard]] f32 log1p(f32 x) noexcept;
[[nodiscard]] f32 log10(f32 x) noexcept;

}  // namespace fp

}  // namespace cy::determinism
