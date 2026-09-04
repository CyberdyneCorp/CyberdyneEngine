// Which SIMD backends this build contains. Task 3.1.3. See include/cy/core/math/simd.h.
//
// These four functions are the only part of the SIMD abstraction that is not a header: everything
// else is inline intrinsics, and this is the reflection over what the preprocessor decided.

#include <cy/core/math/simd.h>

namespace cy::math::simd {

const char* backend_name(Backend backend) noexcept {
    switch (backend) {
        case Backend::Scalar:
            return "scalar";
        case Backend::Sse:
            return "sse";
        case Backend::Avx2:
            return "avx2";
        case Backend::Neon:
            return "neon";
        case Backend::kCount:
            break;
    }
    return "unknown";
}

bool backend_compiled(Backend backend) noexcept {
    // Which arms are identical is a property of the target, not of the code: on a baseline x86-64
    // build Scalar and Sse both answer true and Avx2 and Neon both answer false, and on an AVX2
    // ARM-less build a different pair coincides. Each case answers a different question and the
    // preprocessor decides the answer, so merging the ones that happen to agree here would merge
    // cases that disagree elsewhere.
    // NOLINTBEGIN(bugprone-branch-clone)
    switch (backend) {
        case Backend::Scalar:
            // Always. That is the point of it (design.md §5).
            return true;
        case Backend::Sse:
#if defined(CY_MATH_HAS_SSE)
            return true;
#else
            return false;
#endif
        case Backend::Avx2:
            // Present only in a build that was told to target AVX2 — `-mavx2`, `-march=x86-64-v3`
            // or better. A default x86-64 build is compiled for the baseline instruction set and
            // does not contain this code, and saying so rather than reporting the 128-bit path
            // under a 256-bit name is what keeps a benchmark from claiming a width it does not
            // have.
#if defined(CY_MATH_HAS_AVX2)
            return true;
#else
            return false;
#endif
        case Backend::Neon:
#if defined(CY_MATH_HAS_NEON)
            return true;
#else
            return false;
#endif
        case Backend::kCount:
            break;
    }
    // NOLINTEND(bugprone-branch-clone)
    return false;
}

Backend active_backend() noexcept {
    // What the batch functions run, which on x86 is the 128-bit path even in a build that contains
    // the 256-bit one — see the measurement in batch.cpp. Reporting the widest *compiled* backend
    // here instead would be a true statement that every reader would draw the wrong conclusion
    // from.
    return ActiveOps::kBackend;
}

usize compiled_backends(Backend* out, usize capacity) noexcept {
    usize count = 0;
    for (u32 i = 0; i < static_cast<u32>(Backend::kCount); ++i) {
        const auto backend = static_cast<Backend>(i);
        if (!backend_compiled(backend)) {
            continue;
        }
        if (count < capacity) {
            out[count] = backend;
        }
        ++count;
    }
    return count;
}

}  // namespace cy::math::simd
