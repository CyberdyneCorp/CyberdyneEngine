// SPDX-License-Identifier: MIT
// The deterministic math entries of ABI 1.8. openspec/changes/add-deterministic-math stage 8.
//
// Each scalar entry is one call into `cy::detmath`, raw value in and raw value out, so a module in
// another language evaluates the engine's polynomial rather than carrying a second one (design
// §13). They are pure, so they take no engine handle, check no phase and cannot fail: every
// function is defined for every input (functions.h states the out-of-domain answers).
// `detmath_evaluate` is the span form, and the only one that can be called wrongly.

#include <cy/abi/errors.h>
#include <cy/core/detmath/fixed.h>
#include <cy/core/detmath/functions.h>
#include <cy/core/detmath/version.h>

#include "thunks.h"

namespace cy::abi::game {
namespace {

namespace dm = cy::detmath;

[[nodiscard]] dm::Fixed fx(CyFixed raw) noexcept {
    return dm::Fixed::from_raw(raw);
}

[[nodiscard]] dm::Angle an(CyAngle raw) noexcept {
    return dm::Angle::from_raw(raw);
}

/// One function over one slot: angles are the slot's low 32 bits in, and zero-extended out.
[[nodiscard]] CyFixed apply(CyDetmathFunction function, CyFixed x, CyFixed y) noexcept {
    const auto angle = static_cast<CyAngle>(static_cast<u64>(x));
    switch (function) {
        case CY_DETMATH_SQRT:
            return detmath_sqrt(x);
        case CY_DETMATH_SIN:
            return detmath_sin(angle);
        case CY_DETMATH_COS:
            return detmath_cos(angle);
        case CY_DETMATH_TAN:
            return detmath_tan(angle);
        case CY_DETMATH_ATAN:
            return detmath_atan(x);
        case CY_DETMATH_ATAN2:
            return detmath_atan2(x, y);
        case CY_DETMATH_ASIN:
            return detmath_asin(x);
        case CY_DETMATH_ACOS:
            return detmath_acos(x);
        case CY_DETMATH_EXP2:
            return detmath_exp2(x);
        case CY_DETMATH_LOG2:
            return detmath_log2(x);
        case CY_DETMATH_EXP:
            return detmath_exp(x);
        case CY_DETMATH_LOG:
            return detmath_log(x);
        case CY_DETMATH_POW:
            return detmath_pow(x, y);
    }
    return 0;
}

[[nodiscard]] bool binary(CyDetmathFunction function) noexcept {
    return function == CY_DETMATH_ATAN2 || function == CY_DETMATH_POW;
}

}  // namespace

uint32_t detmath_kernel_version() {
    return dm::kKernelVersion;
}

CyFixed detmath_sqrt(CyFixed x) {
    return dm::sqrt(fx(x)).raw;
}

CyFixed detmath_sin(CyAngle a) {
    return dm::sin(an(a)).raw;
}

CyFixed detmath_cos(CyAngle a) {
    return dm::cos(an(a)).raw;
}

CyFixed detmath_tan(CyAngle a) {
    return dm::tan(an(a)).raw;
}

CyAngle detmath_atan(CyFixed x) {
    return dm::atan(fx(x)).raw;
}

CyAngle detmath_atan2(CyFixed y, CyFixed x) {
    return dm::atan2(fx(y), fx(x)).raw;
}

CyAngle detmath_asin(CyFixed x) {
    return dm::asin(fx(x)).raw;
}

CyAngle detmath_acos(CyFixed x) {
    return dm::acos(fx(x)).raw;
}

CyFixed detmath_exp2(CyFixed x) {
    return dm::exp2(fx(x)).raw;
}

CyFixed detmath_log2(CyFixed x) {
    return dm::log2(fx(x)).raw;
}

CyFixed detmath_exp(CyFixed x) {
    return dm::exp(fx(x)).raw;
}

CyFixed detmath_log(CyFixed x) {
    return dm::log(fx(x)).raw;
}

CyFixed detmath_pow(CyFixed x, CyFixed y) {
    return dm::pow(fx(x), fx(y)).raw;
}

CyResult detmath_evaluate(uint32_t function, const CyFixed* x, const CyFixed* y, CyFixed* out,
                          uint64_t count) {
    if (function > static_cast<uint32_t>(CY_DETMATH_POW)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "detmath_evaluate: the function is not a CyDetmathFunction");
    }
    const auto which = static_cast<CyDetmathFunction>(function);
    if (count != 0 && (x == nullptr || out == nullptr || (binary(which) && y == nullptr))) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "detmath_evaluate: a span is null; atan2 and pow need y as well as x");
    }
    for (uint64_t index = 0; index < count; ++index) {
        out[index] = apply(which, x[index], binary(which) ? y[index] : 0);
    }
    clear_last_error();
    return CY_RESULT_OK;
}

}  // namespace cy::abi::game
