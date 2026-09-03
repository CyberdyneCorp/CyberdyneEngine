#pragma once
// The thin SIMD abstraction, and the scalar reference every path is measured against. Task 3.1.3.
//
// design.md §5 — "SIMD is an implementation detail with a scalar reference": **one scalar
// reference implementation, always compiled and always tested, plus SIMD paths selected at build
// time. Every SIMD path is tested against the scalar reference for bit-identical results where the
// operation is exact, and within a stated tolerance where it is not.**
//
// The reference is not a fallback that only ARM users see. It is compiled into every build of the
// engine on every platform, it is what tests/test_simd.cpp compares against, and it is what
// `simulation-and-determinism` will need at M9 when the question becomes "does this platform agree
// with that one" — a question that has no meaning unless there is one implementation both are
// compared to.
//
// SELECTION IS AT BUILD TIME, NOT RUN TIME. There is no CPUID dispatch here. A build targets an
// instruction set, `CY_MATH_SIMD_BACKEND` records which, and the batch functions in batch.h call
// that one. Runtime dispatch buys the ability to ship one binary for several instruction sets; it
// costs an indirect call on every batch and, more importantly, it makes "which code ran" a
// property of the host rather than of the build, which is the opposite of what a determinism
// requirement wants. If a shipping configuration later needs it, it belongs in one dispatch table
// in batch.cpp and not in this header.
//
// WHAT IS DELIBERATELY 4-WIDE. `core-math` names SSE4.2, AVX2 and NEON as backends. The three
// implemented here are the scalar reference, a 128-bit x86 path (SSE2 baseline, using the SSE4.1
// instructions when the target has them) and a 128-bit NEON path. An AVX2 path is 8-wide and needs
// an eight-lane type rather than a wider `Float4`; the batch functions are written over a lane
// count so that adding one is a new `Ops` struct and a new instantiation, not a rewrite. Until it
// exists, `backend_compiled(Backend::Avx2)` answers false rather than quietly reporting a 128-bit
// path under a 256-bit name.

#include <cy/core/base/types.h>
#include <cy/core/math/scalar.h>

#include <cstring>

// --- Feature detection --------------------------------------------------------------------------
//
// From what the compiler was told to target, never from what the host happens to have. Define
// CY_MATH_FORCE_SCALAR to build the reference alone — that is how the scalar path is exercised on a
// machine that has SIMD, and it is what a determinism build would use.

#if !defined(CY_MATH_FORCE_SCALAR)
#    if defined(__SSE2__) || (defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#        define CY_MATH_HAS_SSE 1
#    endif
#    if defined(__SSE4_1__)
#        define CY_MATH_HAS_SSE41 1
#    endif
#    if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#        define CY_MATH_HAS_NEON 1
#    endif
#endif

#if defined(CY_MATH_HAS_SSE)
#    include <emmintrin.h>  // SSE2
#    if defined(CY_MATH_HAS_SSE41)
#        include <smmintrin.h>  // SSE4.1
#    endif
#endif

#if defined(CY_MATH_HAS_NEON)
#    include <arm_neon.h>
#endif

namespace cy::math::simd {

/// The backends the engine knows about. `Avx2` is listed and is not implemented; see the header
/// comment. `backend_compiled()` is the honest answer for every entry.
enum class Backend : u32 {
    Scalar = 0,
    Sse,
    Avx2,
    Neon,
    kCount,
};

/// The enumerator's own spelling, for a diagnostic or a test's failure message. Never null.
[[nodiscard]] const char* backend_name(Backend backend) noexcept;

/// Whether this build contains that backend's code at all.
[[nodiscard]] bool backend_compiled(Backend backend) noexcept;

/// The backend the batch functions in batch.h actually use in this build. Always a compiled one,
/// and `Backend::Scalar` in a build that has no SIMD or that defined CY_MATH_FORCE_SCALAR.
[[nodiscard]] Backend active_backend() noexcept;

/// Every compiled backend, written into `out` and returned as a count. `Scalar` is always the
/// first entry, because it is always present. Used by tests/test_simd.cpp to iterate the paths
/// that exist in this build rather than the paths that exist in principle.
[[nodiscard]] usize compiled_backends(Backend* out, usize capacity) noexcept;

// --- The scalar reference
// -------------------------------------------------------------------------
//
// Four floats and the obvious arithmetic. It is written to be *readable*, because its job is to be
// the definition of what the other backends must produce; a clever scalar reference would only
// raise the question of whether the reference itself is right.

namespace reference {

struct Float4 {
    f32 v[4];
};

[[nodiscard]] inline Float4 splat(f32 s) noexcept {
    return Float4{{s, s, s, s}};
}
[[nodiscard]] inline Float4 zero() noexcept {
    return Float4{{0.0f, 0.0f, 0.0f, 0.0f}};
}
[[nodiscard]] inline Float4 set(f32 a, f32 b, f32 c, f32 d) noexcept {
    return Float4{{a, b, c, d}};
}

/// Unaligned load, because the engine's arrays of `Vec3` are 12-byte-strided and nothing in them is
/// 16-byte aligned. An aligned variant would be faster on hardware that no longer exists.
[[nodiscard]] inline Float4 load(const f32* p) noexcept {
    Float4 r;
    std::memcpy(r.v, p, sizeof(r.v));
    return r;
}

inline void store(f32* p, Float4 a) noexcept {
    std::memcpy(p, a.v, sizeof(a.v));
}

[[nodiscard]] inline Float4 add(Float4 a, Float4 b) noexcept {
    return Float4{{a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2], a.v[3] + b.v[3]}};
}
[[nodiscard]] inline Float4 sub(Float4 a, Float4 b) noexcept {
    return Float4{{a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2], a.v[3] - b.v[3]}};
}
[[nodiscard]] inline Float4 mul(Float4 a, Float4 b) noexcept {
    return Float4{{a.v[0] * b.v[0], a.v[1] * b.v[1], a.v[2] * b.v[2], a.v[3] * b.v[3]}};
}
[[nodiscard]] inline Float4 div(Float4 a, Float4 b) noexcept {
    return Float4{{a.v[0] / b.v[0], a.v[1] / b.v[1], a.v[2] / b.v[2], a.v[3] / b.v[3]}};
}
/// Minimum, with the **selection rule spelled out**: the first operand when it compares less, the
/// second otherwise.
///
/// That is not the same as `cy::math::min`, which returns the *first* operand on a tie, and the
/// difference is deliberate. `MINPS` is defined as `(a < b) ? a : b`, so writing the reference the
/// same way makes the two bit-identical even where the values compare equal but differ in bits
/// (+0.0 against -0.0) and where the comparison is false for both orderings (a NaN operand). The
/// alternative — a "cleaner" reference plus a documented tolerance for signed zero — would weaken
/// the one guarantee this file exists to provide. This cost one test failure to discover, which is
/// exactly what the comparison is for.
///
/// NEON's `VMIN`/`FMIN` uses a different tie rule for signed zeros and for NaN, so the ARM backend
/// is expected to diverge in those two cases. It is unverified either way — there is no ARM host in
/// this milestone — and the first ARM CI run is what will settle it.
[[nodiscard]] inline Float4 min(Float4 a, Float4 b) noexcept {
    return Float4{{a.v[0] < b.v[0] ? a.v[0] : b.v[0], a.v[1] < b.v[1] ? a.v[1] : b.v[1],
                   a.v[2] < b.v[2] ? a.v[2] : b.v[2], a.v[3] < b.v[3] ? a.v[3] : b.v[3]}};
}

/// Maximum, by the mirror of `min`'s rule: the first operand when it compares greater.
[[nodiscard]] inline Float4 max(Float4 a, Float4 b) noexcept {
    return Float4{{a.v[0] > b.v[0] ? a.v[0] : b.v[0], a.v[1] > b.v[1] ? a.v[1] : b.v[1],
                   a.v[2] > b.v[2] ? a.v[2] : b.v[2], a.v[3] > b.v[3] ? a.v[3] : b.v[3]}};
}

/// Multiply-add, written as two separate operations on purpose.
///
/// A fused multiply-add rounds once where this rounds twice, so the two give different answers.
/// That is a *correct* optimisation the standard permits and a compiler will take unless told not
/// to, which is why src/CMakeLists.txt compiles this module with contraction disabled: without it,
/// the scalar reference and the SIMD path could differ by a ULP for reasons neither implementation
/// mentions, and the bit-identity test would fail on a compiler upgrade rather than on a code
/// change. A future FMA backend is welcome; it must declare itself as a separate backend with a
/// stated tolerance rather than silently replacing this.
[[nodiscard]] inline Float4 madd(Float4 a, Float4 b, Float4 c) noexcept {
    return add(mul(a, b), c);
}

/// Bit `i` of the result is set when `a[i] >= b[i]`. The frustum cull reads it as four rejection
/// answers at once.
[[nodiscard]] inline u32 mask_ge(Float4 a, Float4 b) noexcept {
    u32 mask = 0;
    for (u32 i = 0; i < 4; ++i) {
        mask |= (a.v[i] >= b.v[i] ? 1u : 0u) << i;
    }
    return mask;
}

[[nodiscard]] inline f32 lane(Float4 a, u32 i) noexcept {
    return a.v[i];
}

[[nodiscard]] inline f32 hsum(Float4 a) noexcept {
    return (a.v[0] + a.v[1]) + (a.v[2] + a.v[3]);
}

}  // namespace reference

// --- The 128-bit x86 backend
// -----------------------------------------------------------------------

#if defined(CY_MATH_HAS_SSE)
namespace sse {

struct Float4 {
    __m128 v;
};

[[nodiscard]] inline Float4 splat(f32 s) noexcept {
    return Float4{_mm_set1_ps(s)};
}
[[nodiscard]] inline Float4 zero() noexcept {
    return Float4{_mm_setzero_ps()};
}
/// Lane order matches the reference: `set(a, b, c, d)` puts `a` in lane 0. `_mm_set_ps` takes its
/// arguments in the opposite order, which is the single most common way to get an SSE port subtly
/// wrong, so it is reversed here once rather than at every call site.
[[nodiscard]] inline Float4 set(f32 a, f32 b, f32 c, f32 d) noexcept {
    return Float4{_mm_set_ps(d, c, b, a)};
}
[[nodiscard]] inline Float4 load(const f32* p) noexcept {
    return Float4{_mm_loadu_ps(p)};
}
inline void store(f32* p, Float4 a) noexcept {
    _mm_storeu_ps(p, a.v);
}

[[nodiscard]] inline Float4 add(Float4 a, Float4 b) noexcept {
    return Float4{_mm_add_ps(a.v, b.v)};
}
[[nodiscard]] inline Float4 sub(Float4 a, Float4 b) noexcept {
    return Float4{_mm_sub_ps(a.v, b.v)};
}
[[nodiscard]] inline Float4 mul(Float4 a, Float4 b) noexcept {
    return Float4{_mm_mul_ps(a.v, b.v)};
}
[[nodiscard]] inline Float4 div(Float4 a, Float4 b) noexcept {
    return Float4{_mm_div_ps(a.v, b.v)};
}

/// The hardware instruction, used as-is: `reference::min` was written to match its selection rule
/// rather than the other way round, so the two agree bit-for-bit including on signed zeros and NaN.
[[nodiscard]] inline Float4 min(Float4 a, Float4 b) noexcept {
    return Float4{_mm_min_ps(a.v, b.v)};
}
[[nodiscard]] inline Float4 max(Float4 a, Float4 b) noexcept {
    return Float4{_mm_max_ps(a.v, b.v)};
}

/// Two roundings, exactly as the reference does. `_mm_fmadd_ps` would round once and would not be
/// bit-identical; see the comment on `reference::madd`.
[[nodiscard]] inline Float4 madd(Float4 a, Float4 b, Float4 c) noexcept {
    return Float4{_mm_add_ps(_mm_mul_ps(a.v, b.v), c.v)};
}

[[nodiscard]] inline u32 mask_ge(Float4 a, Float4 b) noexcept {
    return static_cast<u32>(_mm_movemask_ps(_mm_cmpge_ps(a.v, b.v)));
}

[[nodiscard]] inline f32 lane(Float4 a, u32 i) noexcept {
    alignas(16) f32 tmp[4];
    _mm_store_ps(tmp, a.v);
    return tmp[i];
}

[[nodiscard]] inline f32 hsum(Float4 a) noexcept {
    // Summed in the same association order as the reference — (0+1) + (2+3) — because
    // floating-point addition is not associative and a different order is a different answer.
    alignas(16) f32 tmp[4];
    _mm_store_ps(tmp, a.v);
    return (tmp[0] + tmp[1]) + (tmp[2] + tmp[3]);
}

}  // namespace sse
#endif  // CY_MATH_HAS_SSE

// --- The 128-bit ARM backend
// ------------------------------------------------------------------------
//
// UNVERIFIED. There is no ARM64 host in this milestone's environment, so this path has never been
// compiled or run. It is written from the NEON intrinsics reference and is structured identically
// to the SSE path so that the diff between them is reviewable; the first ARM CI job is what will
// turn it from plausible into tested. It is listed as a compiled backend only where the compiler
// actually defines __ARM_NEON, so an x86 build cannot report it by mistake.

#if defined(CY_MATH_HAS_NEON)
namespace neon {

struct Float4 {
    float32x4_t v;
};

[[nodiscard]] inline Float4 splat(f32 s) noexcept {
    return Float4{vdupq_n_f32(s)};
}
[[nodiscard]] inline Float4 zero() noexcept {
    return Float4{vdupq_n_f32(0.0f)};
}
[[nodiscard]] inline Float4 set(f32 a, f32 b, f32 c, f32 d) noexcept {
    const f32 lanes[4] = {a, b, c, d};
    return Float4{vld1q_f32(lanes)};
}
[[nodiscard]] inline Float4 load(const f32* p) noexcept {
    return Float4{vld1q_f32(p)};
}
inline void store(f32* p, Float4 a) noexcept {
    vst1q_f32(p, a.v);
}

[[nodiscard]] inline Float4 add(Float4 a, Float4 b) noexcept {
    return Float4{vaddq_f32(a.v, b.v)};
}
[[nodiscard]] inline Float4 sub(Float4 a, Float4 b) noexcept {
    return Float4{vsubq_f32(a.v, b.v)};
}
[[nodiscard]] inline Float4 mul(Float4 a, Float4 b) noexcept {
    return Float4{vmulq_f32(a.v, b.v)};
}
[[nodiscard]] inline Float4 div(Float4 a, Float4 b) noexcept {
    return Float4{vdivq_f32(a.v, b.v)};
}
[[nodiscard]] inline Float4 min(Float4 a, Float4 b) noexcept {
    return Float4{vminq_f32(a.v, b.v)};
}
[[nodiscard]] inline Float4 max(Float4 a, Float4 b) noexcept {
    return Float4{vmaxq_f32(a.v, b.v)};
}

/// `vmlaq_f32` is permitted to fuse. Written as a separate multiply and add for the same reason the
/// SSE path avoids `_mm_fmadd_ps`: the reference rounds twice, and a fused path is a different
/// backend with a stated tolerance, not a faster spelling of this one.
[[nodiscard]] inline Float4 madd(Float4 a, Float4 b, Float4 c) noexcept {
    return Float4{vaddq_f32(vmulq_f32(a.v, b.v), c.v)};
}

[[nodiscard]] inline u32 mask_ge(Float4 a, Float4 b) noexcept {
    const uint32x4_t cmp = vcgeq_f32(a.v, b.v);
    // NEON has no movemask. Reduce the four all-ones lanes to four bits by masking with the lane
    // weights and adding across, which is the standard idiom and is branch-free.
    const uint32x4_t weights = {1u, 2u, 4u, 8u};
    return vaddvq_u32(vandq_u32(cmp, weights));
}

[[nodiscard]] inline f32 lane(Float4 a, u32 i) noexcept {
    f32 tmp[4];
    vst1q_f32(tmp, a.v);
    return tmp[i];
}

[[nodiscard]] inline f32 hsum(Float4 a) noexcept {
    f32 tmp[4];
    vst1q_f32(tmp, a.v);
    return (tmp[0] + tmp[1]) + (tmp[2] + tmp[3]);
}

}  // namespace neon
#endif  // CY_MATH_HAS_NEON

// --- Backend tags
// ------------------------------------------------------------------------------------
//
// What the batch algorithms in batch.cpp are templated over. Each tag names its `Vec` type and
// forwards the whole operation set as static members.
//
// Static members rather than argument-dependent lookup on the free functions above, for one
// concrete reason: `load`, `splat`, `set` and `zero` take no argument of the backend's own type, so
// ADL cannot find them and an unqualified call inside a template would resolve to whichever
// overload happened to be visible. Forwarding every operation — not only those four — keeps the
// algorithm reading as `Ops::mul(a, b)` throughout, so which backend a line belongs to is never a
// question. The forwarding is the entire cost of the abstraction: no virtual dispatch, no macro
// switch, one algorithm compiled once per backend.

struct ReferenceOps {
    using Vec = reference::Float4;
    static constexpr Backend kBackend = Backend::Scalar;

    static Vec splat(f32 s) noexcept { return reference::splat(s); }
    static Vec zero() noexcept { return reference::zero(); }
    static Vec set(f32 a, f32 b, f32 c, f32 d) noexcept { return reference::set(a, b, c, d); }
    static Vec load(const f32* p) noexcept { return reference::load(p); }
    static void store(f32* p, Vec a) noexcept { reference::store(p, a); }
    static Vec add(Vec a, Vec b) noexcept { return reference::add(a, b); }
    static Vec sub(Vec a, Vec b) noexcept { return reference::sub(a, b); }
    static Vec mul(Vec a, Vec b) noexcept { return reference::mul(a, b); }
    static Vec div(Vec a, Vec b) noexcept { return reference::div(a, b); }
    static Vec min(Vec a, Vec b) noexcept { return reference::min(a, b); }
    static Vec max(Vec a, Vec b) noexcept { return reference::max(a, b); }
    static Vec madd(Vec a, Vec b, Vec c) noexcept { return reference::madd(a, b, c); }
    static u32 mask_ge(Vec a, Vec b) noexcept { return reference::mask_ge(a, b); }
    static f32 lane(Vec a, u32 i) noexcept { return reference::lane(a, i); }
    static f32 hsum(Vec a) noexcept { return reference::hsum(a); }
};

#if defined(CY_MATH_HAS_SSE)
struct SseOps {
    using Vec = sse::Float4;
    static constexpr Backend kBackend = Backend::Sse;

    static Vec splat(f32 s) noexcept { return sse::splat(s); }
    static Vec zero() noexcept { return sse::zero(); }
    static Vec set(f32 a, f32 b, f32 c, f32 d) noexcept { return sse::set(a, b, c, d); }
    static Vec load(const f32* p) noexcept { return sse::load(p); }
    static void store(f32* p, Vec a) noexcept { sse::store(p, a); }
    static Vec add(Vec a, Vec b) noexcept { return sse::add(a, b); }
    static Vec sub(Vec a, Vec b) noexcept { return sse::sub(a, b); }
    static Vec mul(Vec a, Vec b) noexcept { return sse::mul(a, b); }
    static Vec div(Vec a, Vec b) noexcept { return sse::div(a, b); }
    static Vec min(Vec a, Vec b) noexcept { return sse::min(a, b); }
    static Vec max(Vec a, Vec b) noexcept { return sse::max(a, b); }
    static Vec madd(Vec a, Vec b, Vec c) noexcept { return sse::madd(a, b, c); }
    static u32 mask_ge(Vec a, Vec b) noexcept { return sse::mask_ge(a, b); }
    static f32 lane(Vec a, u32 i) noexcept { return sse::lane(a, i); }
    static f32 hsum(Vec a) noexcept { return sse::hsum(a); }
};
#endif

#if defined(CY_MATH_HAS_NEON)
struct NeonOps {
    using Vec = neon::Float4;
    static constexpr Backend kBackend = Backend::Neon;

    static Vec splat(f32 s) noexcept { return neon::splat(s); }
    static Vec zero() noexcept { return neon::zero(); }
    static Vec set(f32 a, f32 b, f32 c, f32 d) noexcept { return neon::set(a, b, c, d); }
    static Vec load(const f32* p) noexcept { return neon::load(p); }
    static void store(f32* p, Vec a) noexcept { neon::store(p, a); }
    static Vec add(Vec a, Vec b) noexcept { return neon::add(a, b); }
    static Vec sub(Vec a, Vec b) noexcept { return neon::sub(a, b); }
    static Vec mul(Vec a, Vec b) noexcept { return neon::mul(a, b); }
    static Vec div(Vec a, Vec b) noexcept { return neon::div(a, b); }
    static Vec min(Vec a, Vec b) noexcept { return neon::min(a, b); }
    static Vec max(Vec a, Vec b) noexcept { return neon::max(a, b); }
    static Vec madd(Vec a, Vec b, Vec c) noexcept { return neon::madd(a, b, c); }
    static u32 mask_ge(Vec a, Vec b) noexcept { return neon::mask_ge(a, b); }
    static f32 lane(Vec a, u32 i) noexcept { return neon::lane(a, i); }
    static f32 hsum(Vec a) noexcept { return neon::hsum(a); }
};
#endif

/// The tag the `*_simd` entry points in batch.h are compiled from. Aliased to the reference in a
/// build with no SIMD, so those entry points always exist and a caller never has to `#ifdef`.
#if defined(CY_MATH_HAS_SSE)
using ActiveOps = SseOps;
#elif defined(CY_MATH_HAS_NEON)
using ActiveOps = NeonOps;
#else
using ActiveOps = ReferenceOps;
#endif

}  // namespace cy::math::simd
