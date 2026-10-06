# src/core/detmath/ — layer 0: deterministic math. Fixed-point scalars, binary angles, 128-bit
# intermediates, and transcendentals computed from integer instructions with declared error bounds.
#
# openspec/changes/add-deterministic-math, stages 1 and 2. Governed by `deterministic-math` and by
# `simulation-and-determinism`'s floating-point policy, which requires "deterministic math types
# ... provided as an optional module" for the CrossPlatform and Lockstep profiles.
#
# OPTIONAL MEANS NOTHING LINKS IT BY DEFAULT. A SamePlatform, ReplayStable or None session never
# sees it. A target that links it gets `CY_DETERMINISM_MATH=1` — PUBLIC, unlike the determinism
# module's own definitions, because what it states is a fact about every translation unit that can
# reach these types: `determinism::BuildConfiguration::from_build()` then reports
# `deterministic_math_available`, and `DeterminismConfiguration::require()` stops refusing
# CrossPlatform and Lockstep as a whole and goes on to ask each authoritative subsystem whether it
# meets them (design §12.1).
#
# THE KERNEL SOURCES ARE LISTED SEPARATELY FROM convert.cpp because the tests compile them twice
# more: once with `-mgeneral-regs-only`, where any float in them is a compile error (design §7.3),
# and once with `-mavx2`, to compare one computation at two vector widths (design §10.4).
# convert.cpp is the boundary, and the only file allowed to hold a float.
#
# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
set(CY_DETMATH_KERNEL_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/digest.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/explog.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/overflow.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sqrt.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/trig.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/wide.cpp")
set_property(GLOBAL PROPERTY CY_DETMATH_KERNEL_SOURCES "${CY_DETMATH_KERNEL_SOURCES}")
set_property(GLOBAL PROPERTY CY_DETMATH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

cy_add_module(
    NAME cy_core_detmath
    LAYER core
    ALIAS cy::core-detmath
    TYPE STATIC
    SOURCES
        ${CY_DETMATH_KERNEL_SOURCES}
        src/convert.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEFINITIONS CY_DETERMINISM_MATH=1
    PUBLIC_DEPENDENCIES cy::core-base)

# Contraction off, as src/core/math/CMakeLists.txt sets it and for the same reason, although the
# kernel has nothing to contract: convert.cpp is floating point, and a fused multiply-add in
# `value * 2^32` would be harmless only by accident. MSVC's `/fp:precise` already forbids it.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cy_core_detmath PRIVATE -ffp-contract=off)
elseif(MSVC)
    target_compile_options(cy_core_detmath PRIVATE /fp:precise)
endif()

cy_declare_determinism_profile(cy_core_detmath
    PROFILE CrossPlatform
    REASON "every result is computed from integer instructions, which C++20 defines exactly; the only floating point is the cook-time conversion in convert.cpp, which uses the IEEE-exact set")

if(CY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
