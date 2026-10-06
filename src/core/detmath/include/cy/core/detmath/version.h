#pragma once
// The kernel version. Design §5.4.

#include <cy/core/base/types.h>
#include <cy/core/detmath/config.h>

namespace cy::detmath::inline CY_DETMATH_VARIANT {

/// Bumped whenever ANY function's output changes for ANY input: a coefficient, a reduction, a
/// rounding rule. It works the way `cy::determinism::kMixerVersion` works for random draws.
///
/// It is folded into the kernel digest (digest.h) and published by `determinism.cross_leg`, and the
/// stages that follow record it in replay manifests and lockstep compatibility scopes, so that two
/// peers with different kernels are told so at join or load rather than discovering it as a
/// desync. `integration.detmath_vectors` pins the committed digest to this number: changing the
/// kernel without bumping it, or bumping it without regenerating, turns that suite red.
inline constexpr u32 kKernelVersion = 1;

}  // namespace cy::detmath::inline CY_DETMATH_VARIANT
