#pragma once
// The two compile-time switches of the deterministic math module.
//
// THE VARIANT NAMESPACE. Every symbol of `cy::core-detmath` lives in an inline namespace named by
// `CY_DETMATH_VARIANT`, `v1` unless a build says otherwise. Code that writes `cy::detmath::Fixed`
// never sees it. It exists for the module's own tests: the kernel's translation units are compiled
// a second time with `-mgeneral-regs-only` (so a float in the kernel fails to compile) and a third
// time with `-mavx2` (design §10.4: one computation at two vector widths), each under its own
// variant name, and all three are linked into one test binary that compares their digests. Without
// distinct names the three copies would be one symbol defined three times.
//
// OVERFLOW COUNTING. Arithmetic wraps in every build (design §4.3), so a development build and a
// shipping build compute the same bits. What differs is whether an overflow is NOTICED: in a build
// that defines `CY_DEVELOPMENT` each wrapping operation checks whether it wrapped and reports it to
// the thread's `OverflowGuard`, and in Shipping the check is not compiled at all.

#include <cy/core/base/types.h>

#ifndef CY_DETMATH_VARIANT
#    define CY_DETMATH_VARIANT v1
#endif

namespace cy::detmath::inline CY_DETMATH_VARIANT {

/// Whether wrapping operations report an overflow to the thread's `OverflowGuard`. False in
/// Shipping, where the check is not compiled; the arithmetic is the same either way.
#if defined(CY_DEVELOPMENT)
inline constexpr bool kCountsOverflow = true;
#else
inline constexpr bool kCountsOverflow = false;
#endif

}  // namespace cy::detmath::inline CY_DETMATH_VARIANT
