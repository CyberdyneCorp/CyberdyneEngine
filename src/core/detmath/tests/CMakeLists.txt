# src/core/detmath/tests/ — the deterministic math module's suites. Stages 1 and 2 of
# openspec/changes/add-deterministic-math.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines after src/ is read, so the
# declaration is deferred to the end of the top-level directory's processing and the taxonomy's
# budgets are read back out of tests/ rather than restated. src/core/math/tests/ and
# src/core/determinism/tests/ do the same, for the same reason.
#
# THREE SUITES, SPLIT BY COST AS THE TAXONOMY SPLITS THEM:
#
#   unit.detmath               the arithmetic rules case by case, the 128-bit paths against the
#                              reference, the exact identities, the conversion boundary and the
#                              profile seam. A millisecond of CPU per case.
#   integration.detmath_vectors every committed golden vector and digest, and every function against
#                              the mpmath oracle with its declared bound. Reading and checking
#                              ~30 000 lines and sweeping ~360 000 inputs is not a unit test.
#   integration.detmath_variants the kernel compiled twice more — with `-mgeneral-regs-only` and with
#                              `-mavx2` — in one process, digest against digest.
#
# The committed files are addressed by absolute paths compiled in, never discovered relative to the
# working directory: a test that guesses where its expectation lives passes vacuously when it guesses
# wrong (tests/determinism/CMakeLists.txt states the same rule).

set_property(GLOBAL PROPERTY CY_DETMATH_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# --- The general-registers and AVX2 builds of the kernel -------------------------------------------
#
# DESIGN §7.3: "The kernel cannot contain a float." The kernel's translation units are compiled a
# second time with `-mgeneral-regs-only`, under which GCC and Clang refuse to generate any
# floating-point or vector-register code, so a `float` or a `double` that reaches the kernel fails
# the BUILD. The flag is probed rather than assumed, and a compiler that refuses it is REPORTED, not
# skipped in silence: MSVC has no equivalent, and a leg that cannot run the check says so in its
# configure output.
#
# DESIGN §10.4: the same sources a third time with `-mavx2`, on the pattern of
# src/core/math/CMakeLists.txt's AVX2 suite and under the same two conditions (the compiler accepts
# the flag and the host executes it, which that file has already probed). Integer code at two vector
# widths must agree bit for bit, which is stricter than core-math's "within documented tolerance".
#
# Each copy is compiled under its own `CY_DETMATH_VARIANT` (config.h), so all three link into one
# test binary without a duplicate symbol, and the test calls `cy::detmath::gro::kernel_digest()` and
# `cy::detmath::avx2::kernel_digest()` beside the ordinary build's.
if(CY_BUILD_TESTS)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-mgeneral-regs-only" CY_DETMATH_COMPILER_ACCEPTS_GENERAL_REGS_ONLY)
    if(CY_DETMATH_COMPILER_ACCEPTS_GENERAL_REGS_ONLY)
        message(STATUS "core-detmath: the kernel is compiled with -mgeneral-regs-only "
                       "(integration.detmath_variants); a float in it fails the build")
    else()
        message(STATUS "core-detmath: the kernel's float-free build is NOT RUN here — compiler "
                       "'${CMAKE_CXX_COMPILER_ID}' does not accept -mgeneral-regs-only")
    endif()
    set_property(GLOBAL PROPERTY CY_DETMATH_GENERAL_REGS_ONLY
                 "${CY_DETMATH_COMPILER_ACCEPTS_GENERAL_REGS_ONLY}")
endif()

function(cy_detmath_add_variant name)
    get_property(sources GLOBAL PROPERTY CY_DETMATH_KERNEL_SOURCES)
    get_property(include_dir GLOBAL PROPERTY CY_DETMATH_INCLUDE_DIR)
    add_library(cy_core_detmath_${name} OBJECT ${sources})
    target_include_directories(cy_core_detmath_${name} PRIVATE "${include_dir}")
    target_link_libraries(cy_core_detmath_${name} PRIVATE cy::compile-options cy::core-base)
    target_compile_definitions(cy_core_detmath_${name} PRIVATE CY_DETMATH_VARIANT=${name})
    target_compile_options(cy_core_detmath_${name} PRIVATE ${ARGN} -ffp-contract=off)
endfunction()

function(cy_detmath_declare_tests)
    get_property(cy_detmath_tests_dir GLOBAL PROPERTY CY_DETMATH_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/core/detmath/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
                "the deferral around it should go away.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME detmath
        KIND unit
        SOURCES
            "${cy_detmath_tests_dir}/test_convert.cpp"
            "${cy_detmath_tests_dir}/test_fixed.cpp"
            "${cy_detmath_tests_dir}/test_functions.cpp"
            "${cy_detmath_tests_dir}/test_profile.cpp"
            "${cy_detmath_tests_dir}/test_wide.cpp"
        DEPENDENCIES cy::core-detmath cy::core-determinism cy::core-memory)

    # The profile seam (test_profile.cpp) asserts what `from_build()` reports in this suite, so the
    # suite is covered by a profile the way src/core/determinism/tests/ covers its own: contraction
    # off, checked at configure, and `CY_DETERMINISM_CONTRACTION_OFF` set because it was checked.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(cy_test_unit_detmath PRIVATE -ffp-contract=off)
    elseif(MSVC)
        target_compile_options(cy_test_unit_detmath PRIVATE /fp:precise)
    endif()
    cy_declare_determinism_profile(cy_test_unit_detmath
        PROFILE CrossPlatform
        REASON "it asserts what the profile's build half reports with the deterministic math module linked")

    cy_add_test(
        NAME detmath_vectors
        KIND integration
        SOURCES "${cy_detmath_tests_dir}/test_vectors.cpp"
        DEPENDENCIES cy::core-detmath
        DEFINITIONS
            CY_DETMATH_VECTORS_DIR="${CMAKE_SOURCE_DIR}/tools/detmath/vectors"
            CY_DETMATH_ORACLE_DIR="${CMAKE_SOURCE_DIR}/tools/detmath/oracle")

    set(variant_definitions "")
    set(variant_objects "")
    get_property(general_regs GLOBAL PROPERTY CY_DETMATH_GENERAL_REGS_ONLY)
    if(general_regs)
        cy_detmath_add_variant(gro -mgeneral-regs-only)
        list(APPEND variant_definitions CY_DETMATH_TEST_GRO=1)
        list(APPEND variant_objects cy_core_detmath_gro)
    endif()
    get_property(avx2 GLOBAL PROPERTY CY_MATH_AVX2_SUITE)
    if(avx2)
        cy_detmath_add_variant(avx2 -mavx2)
        list(APPEND variant_definitions CY_DETMATH_TEST_AVX2=1)
        list(APPEND variant_objects cy_core_detmath_avx2)
    endif()

    cy_add_test(
        NAME detmath_variants
        KIND integration
        SOURCES "${cy_detmath_tests_dir}/test_variants.cpp"
        DEPENDENCIES cy::core-detmath
        DEFINITIONS ${variant_definitions})
    # Linking an OBJECT library links its objects: the variant builds join this binary whole.
    if(variant_objects)
        target_link_libraries(cy_test_integration_detmath_variants PRIVATE ${variant_objects})
    endif()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_detmath_declare_tests)
