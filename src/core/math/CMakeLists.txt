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
        src/easing.cpp
        src/geometry.cpp
        src/matrix.cpp
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

# tests/ is read now and declares its suites later; see the comment at the top of it for why.
if(CY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
