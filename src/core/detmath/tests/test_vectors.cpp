// SPDX-License-Identifier: MIT
// THE COMMITTED ANSWERS, AND THE DECLARED BOUNDS. Tasks 1.5, 2.3 and 2.4; design §10.1 and §10.3.
//
// ================================================================================================
// GOLDEN VECTORS AND DIGESTS: CHECKED ON EACH LEG ALONE
// ================================================================================================
//
// tools/detmath/vectors/ holds, per function, the edge cases and the first inputs of the seeded
// sweep with their expected outputs, and digests.txt holds each function's digest over the whole
// sweep. The expectations were computed by tools/detmath/model.py — the rules of design §4.3
// written a second time, in Python integers — so a pass here means this leg's C++ and an
// independent statement of the rules agree on every input. The answer is committed, so a leg that
// diverges fails BY ITSELF, without waiting for the cross-leg comparison.
//
// `CY_DETMATH_RECORD_GOLDEN=1` rewrites the outputs from this build AND FAILS THE RUN, as
// `determinism.golden_replay` does, so new expectations cannot be accepted inside a green job.
//
// ================================================================================================
// THE ORACLE: EVERY DECLARED BOUND, WITH THE WORST ERROR PRINTED BESIDE IT
// ================================================================================================
//
// tools/detmath/oracle/ holds mpmath's value of each function at the quantised input, scaled by
// 2^64. Each function's case asserts the bound functions.h declares and prints the worst error it
// measured, so a reader sees the margin and not only a pass. Every comparison allows one further
// unit of 2^-64 for the reference's own rounding; nothing else is added to a declared bound.
//
// ================================================================================================
// HOW THIS SUITE IS MADE TO FAIL
// ================================================================================================
//
//   * flip the tie rule in `Fixed operator*` to round-half-even, and "every golden vector" goes red
//     at mul.txt's tie lines and in its digest;
//   * flip the last bit of one coefficient of kSin or kAtan in generated/coefficients.h, and the
//     golden vectors and digests of sin, cos and tan (or atan, atan2, asin and acos) go red;
//   * change `kKernelVersion` without regenerating, and "the committed digests" goes red.
// The runs are recorded in src/core/detmath/README.md.

#include "detmath_test.h"

#include <cy/core/detmath/detmath.h>
#include <cy/test/fixtures.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using cy::i64;
using cy::u32;
using cy::u64;
using cy::usize;
using cy::detmath::Angle;
using cy::detmath::Fixed;
using cy::detmath::KernelFunction;
using cy::detmath::U128;
using cy::detmath::WideFixed;
namespace dm = cy::detmath;
namespace wide = cy::detmath::wide;

[[nodiscard]] Fixed fx(u64 bits) noexcept {
    return Fixed::from_raw(static_cast<i64>(bits));
}

[[nodiscard]] Angle an(u64 bits) noexcept {
    return Angle::from_raw(static_cast<u32>(bits));
}

[[nodiscard]] u64 out(Fixed value) noexcept {
    return static_cast<u64>(value.raw);
}

[[nodiscard]] u64 out(Angle value) noexcept {
    return value.raw;
}

using Evaluate = u64 (*)(const u64* inputs);

/// One function the files name: its stem, its arity, and how to evaluate it on raw inputs.
struct Function {
    const char* name;
    usize arity;
    Evaluate evaluate;
};

// clang-format off
constexpr Function kFunctions[] = {
    {"add", 2, [](const u64* in) { return out(fx(in[0]) + fx(in[1])); }},
    {"sub", 2, [](const u64* in) { return out(fx(in[0]) - fx(in[1])); }},
    {"mul", 2, [](const u64* in) { return out(fx(in[0]) * fx(in[1])); }},
    {"div", 2, [](const u64* in) { return out(fx(in[0]) / fx(in[1])); }},
    {"sqrt", 1, [](const u64* in) { return out(dm::sqrt(fx(in[0]))); }},
    {"sqrt_wide", 2, [](const u64* in) { return out(dm::sqrt(WideFixed::from_raw(U128{in[1], in[0]}))); }},
    {"narrow16", 1, [](const u64* in) {
         return static_cast<u64>(static_cast<i64>(dm::Fixed16::narrow(fx(in[0])).raw)); }},
    {"angle_scale", 2, [](const u64* in) { return out(an(in[0]) * fx(in[1])); }},
    {"angle_from_radians", 1, [](const u64* in) { return out(Angle::from_radians(fx(in[0]))); }},
    {"angle_radians", 1, [](const u64* in) { return out(an(in[0]).radians()); }},
    {"sin", 1, [](const u64* in) { return out(dm::sin(an(in[0]))); }},
    {"cos", 1, [](const u64* in) { return out(dm::cos(an(in[0]))); }},
    {"tan", 1, [](const u64* in) { return out(dm::tan(an(in[0]))); }},
    {"atan", 1, [](const u64* in) { return out(dm::atan(fx(in[0]))); }},
    {"atan2", 2, [](const u64* in) { return out(dm::atan2(fx(in[0]), fx(in[1]))); }},
    {"asin", 1, [](const u64* in) { return out(dm::asin(fx(in[0]))); }},
    {"acos", 1, [](const u64* in) { return out(dm::acos(fx(in[0]))); }},
    {"exp2", 1, [](const u64* in) { return out(dm::exp2(fx(in[0]))); }},
    {"log2", 1, [](const u64* in) { return out(dm::log2(fx(in[0]))); }},
    {"exp", 1, [](const u64* in) { return out(dm::exp(fx(in[0]))); }},
    {"log", 1, [](const u64* in) { return out(dm::log(fx(in[0]))); }},
    {"pow", 2, [](const u64* in) { return out(dm::pow(fx(in[0]), fx(in[1]))); }},
};
// clang-format on

static_assert(sizeof(kFunctions) / sizeof(kFunctions[0]) ==
                  static_cast<usize>(KernelFunction::Count),
              "every function the digest covers has a vector file, and the other way round");

[[nodiscard]] const Function* find(std::string_view name) noexcept {
    for (const Function& function : kFunctions) {
        if (name == function.name) {
            return &function;
        }
    }
    return nullptr;
}

[[nodiscard]] bool recording() noexcept {
    const char* flag = std::getenv("CY_DETMATH_RECORD_GOLDEN");
    return flag != nullptr && flag[0] == '1';
}

[[nodiscard]] std::string path_of(const char* directory, const std::string& name) {
    return std::string(directory) + "/" + name;
}

[[nodiscard]] std::string hex16(u64 value) {
    char text[17];
    (void)std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

// --- The oracle ---------------------------------------------------------------------------------

/// What one function's declared bound allows, in units of 2^-64 of the output.
using Allowed = U128 (*)(const u64* inputs, U128 reference_magnitude);

[[nodiscard]] U128 ulps(u64 count) noexcept {
    return wide::shl(wide::from_u64(count), 32);
}

[[nodiscard]] U128 larger(U128 a, U128 b) noexcept {
    return wide::less(a, b) ? b : a;
}

struct Bound {
    const char* name;
    bool angle;          ///< The output is an `Angle`: errors are taken modulo one turn.
    const char* stated;  ///< The bound as functions.h states it, for the report.
    Allowed allowed;
};

// clang-format off
constexpr Bound kBounds[] = {
    {"sqrt", false, "0.5 ulp", [](const u64*, U128) { return wide::from_u64(u64{1} << 31); }},
    {"sin", false, "1 ulp", [](const u64*, U128) { return ulps(1); }},
    {"cos", false, "1 ulp", [](const u64*, U128) { return ulps(1); }},
    {"tan", false, "2 ulp where |tan| <= 1, relative 2^-30 above",
     [](const u64*, U128 r) { return wide::less(U128{0, 1}, r) ? wide::shr(r, 30) : ulps(2); }},
    {"atan", true, "1 ulp", [](const u64*, U128) { return ulps(1); }},
    {"atan2", true, "1 ulp", [](const u64*, U128) { return ulps(1); }},
    {"asin", true, "2 ulp, 2^-15 turn within 2^-16 of +-1", [](const u64* in, U128) {
         const i64 x = static_cast<i64>(in[0]);
         const i64 edge = Fixed::kOneRaw - (i64{1} << 16);
         return (x <= edge && x >= -edge) ? ulps(2) : wide::from_u64(u64{1} << 49); }},
    {"acos", true, "2 ulp, 2^-15 turn within 2^-16 of +-1", [](const u64* in, U128) {
         const i64 x = static_cast<i64>(in[0]);
         const i64 edge = Fixed::kOneRaw - (i64{1} << 16);
         return (x <= edge && x >= -edge) ? ulps(2) : wide::from_u64(u64{1} << 49); }},
    {"exp2", false, "max(1 ulp, relative 2^-36)",
     [](const u64*, U128 r) { return larger(ulps(1), wide::shr(r, 36)); }},
    {"log2", false, "2 ulp", [](const u64*, U128) { return ulps(2); }},
    {"exp", false, "max(2 ulp, relative 2^-34)",
     [](const u64*, U128 r) { return larger(ulps(2), wide::shr(r, 34)); }},
    {"log", false, "3 ulp", [](const u64*, U128) { return ulps(3); }},
    {"pow", false, "max(2 ulp, relative 2^-34 + |y| 2^-58)", [](const u64* in, U128 r) {
         const auto y = static_cast<i64>(in[1]);
         const u64 magnitude_y = y < 0 ? u64{0} - static_cast<u64>(y) : static_cast<u64>(y);
         // |y| 2^-58 |r| with |y| = magnitude_y 2^-32: (|r| >> 58) |y_raw| >> 32.
         const U128 amplified = wide::shr(wide::mul_u64(wide::shr(r, 58).lo, magnitude_y), 32);
         return larger(ulps(2), wide::add(wide::shr(r, 34), amplified)); }},
};
// clang-format on

/// `value` in ulps of Q32.32, for printing: a 2^-64-scaled magnitude divided by 2^32.
[[nodiscard]] double in_ulps(U128 magnitude) noexcept {
    return ((static_cast<double>(magnitude.hi) * 18446744073709551616.0) +
            static_cast<double>(magnitude.lo)) /
           4294967296.0;
}

struct OracleResult {
    usize cases = 0;
    usize violations = 0;
    double worst_ulps = 0.0;
    double worst_fraction_of_bound = 0.0;
    std::string worst_line;
};

/// Every line of one oracle file against its function and bound.
[[nodiscard]] OracleResult check_oracle(const Function& function, const Bound& bound,
                                        const std::string& text) {
    OracleResult result;
    for (const std::string_view line : cy::detmath_test::data_lines(text)) {
        const auto fields = cy::detmath_test::split(line);
        if (fields.size() != function.arity + 1) {
            ++result.violations;
            result.worst_line = std::string(line) + "  (malformed)";
            continue;
        }
        u64 inputs[2] = {0, 0};
        for (usize index = 0; index < function.arity; ++index) {
            U128 parsed{};
            if (!cy::detmath_test::parse_hex(fields[index], parsed)) {
                ++result.violations;
            }
            inputs[index] = parsed.lo;
        }
        ++result.cases;
        const u64 got = function.evaluate(inputs);
        const std::string_view reference = fields[function.arity];
        if (reference == "max" || reference == "min") {
            const u64 expected = static_cast<u64>(reference == "max" ? INT64_MAX : INT64_MIN);
            if (got != expected) {
                ++result.violations;
                result.worst_line = std::string(line) + "  got " + hex16(got);
            }
            continue;
        }
        U128 expected{};
        if (!cy::detmath_test::parse_signed_hex(reference, expected)) {
            ++result.violations;
            continue;
        }
        U128 error{};
        if (bound.angle) {
            // Turns, modulo one turn: the difference as a signed 64-bit quantity.
            const auto difference = static_cast<i64>((got << 32) - expected.lo);
            error = wide::from_u64(difference < 0 ? u64{0} - static_cast<u64>(difference)
                                                  : static_cast<u64>(difference));
        } else {
            const U128 scaled = wide::shl(wide::from_i64(static_cast<i64>(got)), 32);
            error = wide::magnitude(wide::sub(scaled, expected));
        }
        const U128 allowed =
            wide::add(bound.allowed(inputs, wide::magnitude(expected)), U128{1, 0});
        const double fraction = in_ulps(error) / std::max(in_ulps(allowed), 1e-30);
        if (fraction > result.worst_fraction_of_bound) {
            result.worst_fraction_of_bound = fraction;
            result.worst_ulps = in_ulps(error);
            result.worst_line = std::string(line) + "  got " + hex16(got);
        }
        if (wide::less(allowed, error)) {
            ++result.violations;
        }
    }
    return result;
}

}  // namespace

CY_TEST_CASE("detmath vectors: every golden vector is reproduced on this leg") {
    for (const Function& function : kFunctions) {
        CY_TEST_INFO("function " << std::string(function.name));
        const std::string path =
            path_of(CY_DETMATH_VECTORS_DIR, std::string(function.name) + ".txt");
        std::string text;
        CY_REQUIRE_MESSAGE(cy::test::read_file(path, text), "cannot read " << path);

        std::string rewritten;
        usize lines = 0;
        usize mismatches = 0;
        std::string first_mismatch;
        usize start = 0;
        while (start < text.size()) {
            usize end = text.find('\n', start);
            end = end == std::string::npos ? text.size() : end;
            const std::string_view line(text.data() + start, end - start);
            start = end + 1;
            if (line.empty() || line[0] == '#') {
                rewritten.append(line).append("\n");
                continue;
            }
            const auto fields = cy::detmath_test::split(line);
            CY_REQUIRE_EQ(fields.size(), function.arity + 2);  // edge|seed, inputs, output
            u64 inputs[2] = {0, 0};
            U128 parsed{};
            for (usize index = 0; index < function.arity; ++index) {
                CY_REQUIRE(cy::detmath_test::parse_hex(fields[index + 1], parsed));
                inputs[index] = parsed.lo;
            }
            CY_REQUIRE(cy::detmath_test::parse_hex(fields[function.arity + 1], parsed));
            const u64 got = function.evaluate(inputs);
            ++lines;
            if (got != parsed.lo) {
                ++mismatches;
                if (first_mismatch.empty()) {
                    first_mismatch = std::string(line) + "  got " + hex16(got);
                }
            }
            rewritten.append(line.substr(0, line.size() - 16)).append(hex16(got)).append("\n");
        }
        CY_CHECK_GT(lines, 256U);
        CY_CHECK_MESSAGE(mismatches == 0, std::string(function.name)
                                              << ": " << mismatches << " of " << lines
                                              << " outputs differ; first: " << first_mismatch);
        if (recording()) {
            CY_CHECK(cy::test::write_file(path, rewritten));
        }
    }
    CY_CHECK_MESSAGE(!recording(),
                     "CY_DETMATH_RECORD_GOLDEN=1 rewrote tools/detmath/vectors/ from "
                     "this build. Review the diff, bump detmath::kKernelVersion if an "
                     "output moved, and run again without it.");
}

CY_TEST_CASE("detmath vectors: the committed digests are this leg's") {
    std::string text;
    CY_REQUIRE(cy::test::read_file(path_of(CY_DETMATH_VECTORS_DIR, "digests.txt"), text));
    u64 committed[static_cast<usize>(KernelFunction::Count)] = {};
    u64 committed_kernel = 0;
    u64 version = 0;
    u64 sweep = 0;
    for (const std::string_view line : cy::detmath_test::data_lines(text)) {
        const auto fields = cy::detmath_test::split(line);
        CY_REQUIRE_EQ(fields.size(), 2U);
        U128 value{};
        const bool decimal = fields[0] == "kernel-version" || fields[0] == "sweep-count";
        if (decimal) {
            const u64 number = std::strtoull(std::string(fields[1]).c_str(), nullptr, 10);
            (fields[0] == "kernel-version" ? version : sweep) = number;
            continue;
        }
        CY_REQUIRE(cy::detmath_test::parse_hex(fields[1], value));
        if (fields[0] == "kernel") {
            committed_kernel = value.lo;
            continue;
        }
        bool known = false;
        for (u32 index = 0; index < static_cast<u32>(KernelFunction::Count); ++index) {
            if (fields[0] == dm::kernel_function_name(static_cast<KernelFunction>(index))) {
                committed[index] = value.lo;
                known = true;
            }
        }
        CY_CHECK_MESSAGE(known, "digests.txt names an unknown function: " << fields[0]);
    }
    // The version is pinned: the kernel cannot change without its version, and the version cannot
    // move without the committed digests being regenerated for it.
    CY_CHECK_EQ(version, u64{dm::kKernelVersion});
    CY_CHECK_EQ(sweep, u64{dm::kSweepCount});

    u64 folded = dm::digest_fold(dm::kSweepSeed, dm::kKernelVersion);
    std::string rewritten = "kernel-version " + std::to_string(dm::kKernelVersion) + "\n" +
                            "sweep-count " + std::to_string(dm::kSweepCount) + "\n";
    for (u32 index = 0; index < static_cast<u32>(KernelFunction::Count); ++index) {
        const auto function = static_cast<KernelFunction>(index);
        const u64 digest = dm::function_digest(function);
        CY_CHECK_MESSAGE(digest == committed[index], std::string(dm::kernel_function_name(function))
                                                         << ": this leg's digest " << hex16(digest)
                                                         << ", committed "
                                                         << hex16(committed[index]));
        folded = dm::digest_fold(folded, digest);
        rewritten += std::string(dm::kernel_function_name(function)) + " " + hex16(digest) + "\n";
    }
    CY_CHECK_MESSAGE(folded == committed_kernel, "kernel digest " << hex16(folded) << ", committed "
                                                                  << hex16(committed_kernel));
    CY_TEST_MESSAGE("kernel digest " << hex16(folded) << " (version " << dm::kKernelVersion
                                     << ", multiply " << std::string(wide::kNativeMultiply)
                                     << ", divide " << std::string(wide::kNativeDivide) << ")");
    if (recording()) {
        rewritten += "kernel " + hex16(folded) + "\n";
        CY_CHECK(cy::test::write_file(path_of(CY_DETMATH_VECTORS_DIR, "digests.txt"),
                                      "# detmath kernel digests, recorded by "
                                      "integration.detmath_vectors.\n" +
                                          rewritten));
        CY_TEST_FAIL_CHECK("CY_DETMATH_RECORD_GOLDEN=1 rewrote digests.txt; run again without it");
    }
}

CY_TEST_CASE("detmath vectors: every function is within its declared bound of the oracle") {
    for (const Bound& bound : kBounds) {
        const Function* function = find(bound.name);
        CY_REQUIRE(function != nullptr);
        std::string text;
        const std::string path = path_of(CY_DETMATH_ORACLE_DIR, std::string(bound.name) + ".txt");
        CY_REQUIRE_MESSAGE(cy::test::read_file(path, text), "cannot read " << path);
        const OracleResult result = check_oracle(*function, bound, text);
        CY_TEST_MESSAGE(std::string(bound.name)
                        << ": worst " << result.worst_ulps << " ulp over " << result.cases
                        << " cases, " << 100.0 * result.worst_fraction_of_bound
                        << "% of the bound (" << std::string(bound.stated) << ")");
        CY_CHECK_GT(result.cases, 2000U);
        CY_CHECK_MESSAGE(result.violations == 0,
                         std::string(bound.name)
                             << ": " << result.violations << " cases outside "
                             << std::string(bound.stated) << "; worst: " << result.worst_line);
    }
}

CY_TEST_CASE("detmath vectors: the native 128-bit paths are the reference over a long sweep") {
    cy::detmath_test::Rng rng(0x10'5EEDULL);
    usize disagreements = 0;
    for (int index = 0; index < 20000; ++index) {
        const u64 a = static_cast<u64>(rng.scaled());
        const u64 b = static_cast<u64>(rng.scaled()) | 1U;
        const U128 numerator{rng.next(), static_cast<u64>(rng.scaled())};
        const auto expected = wide::reference::divrem(numerator, b);
        const auto native = wide::divrem(numerator, b);
        const auto long_division = wide::long_division::divrem(numerator, b);
        if (!(wide::mul_u64(a, b) == wide::reference::mul_u64(a, b)) ||
            !(native.quotient == expected.quotient) || native.remainder != expected.remainder ||
            !(long_division.quotient == expected.quotient) ||
            long_division.remainder != expected.remainder ||
            wide::isqrt(numerator) != wide::reference::isqrt(numerator)) {
            ++disagreements;
        }
    }
    CY_CHECK_EQ(disagreements, 0U);
}
