# src/core/math/ — layer 0: the engine's vector, matrix, quaternion and transform types, the SIMD
# paths beneath them, and the spatial primitives built on top.
#
# Section 3.1. Governed by `core-math`. The coordinate, depth and unit conventions live here as
# executable tests rather than as prose (design.md §4) — right-handed, Y-up, −Z forward; reversed-Z
# over [0,1] cleared to 0 and compared GreaterEqual; column-major with column vectors; metres,
# seconds, radians. Each test asserts the numeric consequence, not the wording.
#
# Every SIMD path is checked against an always-compiled scalar reference (design.md §5).

# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_core_math
    LAYER core
    ALIAS cy::core-math
    TYPE STATIC
    SOURCES
        src/batch.cpp
        src/bvh.cpp
        src/color.cpp
        src/curve.cpp
        src/delaunay.cpp
        src/easing.cpp
        src/geometry.cpp
        src/hull3d.cpp
        src/matrix.cpp
        src/polygon_boolean.cpp
        src/polygon_offset.cpp
        src/projection.cpp
        src/quat.cpp
        src/random.cpp
        src/shapes.cpp
        src/simd.cpp
        src/spatial.cpp
        src/transform.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base)

# FLOATING-POINT CONTRACTION IS OFF FOR THIS MODULE, AND THAT IS LOAD-BEARING.
#
# design.md §5 requires every SIMD path to be bit-identical to the scalar reference where the
# operation is exact. `a * b + c` is exact in both paths only if neither is allowed to fuse the
# multiply and the add into one instruction that rounds once instead of twice. GCC defaults to
# `-ffp-contract=fast` and Clang to `on`, so without this flag the reference could be compiled with
# an FMA and the SSE path without one — the two would differ by a ULP, the bit-identity test would
# fail, and it would fail on a *compiler upgrade* rather than on a code change, which is the worst
# possible way to learn about it.
#
# PRIVATE, so nothing that merely includes <cy/core/math/...> inherits it: this is a property of how
# this module's own translation units are compiled, not a contract on its callers. The batch
# entry points are non-inline for exactly that reason (see batch.h), so a caller compiles no
# floating-point of ours.
#
# It is also the only platform-conditional code this module adds, and design.md §9 asks for a stated
# reason: MSVC's default `/fp:precise` already forbids contraction, so there is nothing to turn off
# there and no equivalent flag to pass.
if(NOT MSVC)
    target_compile_options(cy_core_math PRIVATE -ffp-contract=off)
endif()

# --- THE 256-BIT BACKEND IS COMPILED AND TESTED BY THE ORDINARY BUILD -------------------------------
#
# M2 task 1.4. `Avx2Ops` is gated on `__AVX2__`, which no preset, no recipe and no CI job set — so
# the backend existed, was documented as verified against the reference, and was compiled by nobody
# on the way in. A SIMD path behind a macro nobody sets is worse than no SIMD path: the first person
# to enable it believes it works, and bit-rot in the meantime is invisible.
#
# The fix is one more test binary rather than a flag on the engine. The engine's own translation
# units stay at the baseline instruction set — a build that targets AVX2 is a decision about the
# hardware a game ships to, not something a test suite gets to make — and one suite is compiled with
# `-mavx2` so that the 256-bit code is compiled, linked and run lane for lane against the scalar
# reference on every ordinary `just test-all`.
#
# src/simd.cpp is compiled *into* that suite rather than taken from the library, because
# `backend_compiled(Backend::Avx2)` is decided by the preprocessor in whichever translation unit
# compiles it: the library's copy answers false, the test's macro says true, and the suite asserts
# the two agree. Nothing else in the module references those four functions, so the archive's copy
# is never pulled in and there is no duplicate definition.
#
# TWO CONDITIONS, both checked rather than assumed, because a suite that cannot run is worse than
# one that does not exist:
#
#   * the compiler accepts the flag — MSVC spells it /arch:AVX2 and has no __builtin_cpu_supports,
#     and Windows is unverified in this milestone, so this is a GCC/Clang arrangement for now;
#   * the *host* executes AVX2. The whole binary is compiled with -mavx2, so a machine without it
#     would meet a SIGILL rather than a skipped test. The probe is compiled at the baseline and run,
#     which also disables the suite when cross-compiling, where try_run cannot answer.
if(CY_BUILD_TESTS AND NOT MSVC)
    include(CheckCXXCompilerFlag)
    include(CheckCXXSourceRuns)

    check_cxx_compiler_flag("-mavx2" CY_MATH_COMPILER_ACCEPTS_MAVX2)
    check_cxx_source_runs("
        #if !defined(__x86_64__) && !defined(__i386__)
        #    error not an x86 target
        #endif
        int main() { return __builtin_cpu_supports(\"avx2\") ? 0 : 1; }
        " CY_MATH_HOST_RUNS_AVX2)

    if(CY_MATH_COMPILER_ACCEPTS_MAVX2 AND CY_MATH_HOST_RUNS_AVX2)
        set_property(GLOBAL PROPERTY CY_MATH_AVX2_SUITE ON)
        message(STATUS "core-math: the 256-bit backend is compiled and tested (integration.math_simd_avx2)")
    else()
        set_property(GLOBAL PROPERTY CY_MATH_AVX2_SUITE OFF)
        message(STATUS
            "core-math: the 256-bit backend is NOT compiled here — "
            "compiler accepts -mavx2: ${CY_MATH_COMPILER_ACCEPTS_MAVX2}, "
            "host executes AVX2: ${CY_MATH_HOST_RUNS_AVX2}")
    endif()
endif()

# tests/ is read now and declares its suites later; see the comment at the top of it for why.
if(CY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
