// SPDX-License-Identifier: MIT
#pragma once
// The kernel digest: every function over a seeded sweep of its inputs, folded in order. Design §10.
//
// One number that is the same on every leg if and only if the kernel computes the same bits there.
// It is checked three ways, each of which can fail on its own:
//
//   on each leg alone     `integration.detmath_vectors` compares it, and each function's own
//                         digest, with tools/detmath/vectors/digests.txt, which
//                         tools/detmath/gen_vectors.py computed from a second implementation of the
//                         rules (tools/detmath/model.py). A leg that diverges fails by itself.
//   between legs          `determinism.cross_leg` publishes it as `detmath-kernel-digest`, and
//                         tools/ci/cross_leg_digests.py compares it across the four CI legs.
//   between builds        `integration.detmath_variants` links the kernel compiled a second time
//                         with `-mgeneral-regs-only` and a third with `-mavx2` into one process and
//                         compares their digests with this build's.
//
// The sweep's inputs come from SplitMix64, whose definition is in digest.cpp and in model.py; the
// draws per function, their order and the fold are part of the digest's definition, and changing
// any of them is a change to the committed number.

#include <cy/core/base/types.h>
#include <cy/core/detmath/config.h>

namespace cy::detmath::inline CY_DETMATH_VARIANT {

/// Every function the digest covers, in digest order.
enum class KernelFunction : u8 {
    Add,
    Subtract,
    Multiply,
    Divide,
    Sqrt,
    SqrtWide,
    NarrowFixed16,
    AngleScale,
    AngleFromRadians,
    AngleRadians,
    Sin,
    Cos,
    Tan,
    Atan,
    Atan2,
    Asin,
    Acos,
    Exp2,
    Log2,
    Exp,
    Log,
    Pow,
    Count,
};

/// The name tools/detmath/ uses for `function`: the vector file's stem and the digests.txt key.
[[nodiscard]] const char* kernel_function_name(KernelFunction function) noexcept;

/// The seed of the sweep: each function's stream starts from it, and the kernel digest's fold too.
inline constexpr u64 kSweepSeed = 0xDE73A70001ULL;

/// Inputs per function in the sweep.
inline constexpr u32 kSweepCount = 16384;

/// The digest of one function over `count` seeded inputs.
[[nodiscard]] u64 function_digest(KernelFunction function, u32 count = kSweepCount) noexcept;

/// The digest of the whole kernel: the version, then every function's digest, in order.
[[nodiscard]] u64 kernel_digest() noexcept;

/// The fold: order-sensitive, so the same outputs in a different order give a different digest.
[[nodiscard]] u64 digest_fold(u64 accumulator, u64 value) noexcept;

}  // namespace cy::detmath::inline CY_DETMATH_VARIANT
