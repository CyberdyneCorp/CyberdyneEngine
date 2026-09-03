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
            // Declared by `core-math` and not implemented; see the header comment in simd.h. Saying
            // so here rather than reporting the 128-bit path under a 256-bit name is what keeps a
            // benchmark from claiming a width it does not have.
            return false;
        case Backend::Neon:
#if defined(CY_MATH_HAS_NEON)
            return true;
#else
            return false;
#endif
        case Backend::kCount:
            break;
    }
    return false;
}

Backend active_backend() noexcept {
    return ActiveOps::kBackend;
}

usize compiled_backends(Backend* out, usize capacity) noexcept {
    usize count = 0;
    for (u32 i = 0; i < static_cast<u32>(Backend::kCount); ++i) {
        const Backend backend = static_cast<Backend>(i);
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
