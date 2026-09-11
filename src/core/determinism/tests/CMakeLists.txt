# src/core/determinism/tests/ — the clock, the streams, the classification and the hash.
# Tasks 4.2.1 through 4.2.7.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred to the end of the top-level directory's processing, by which time the
# taxonomy has been declared, and the budgets are read back out of tests/ rather than restated: a
# budget copied into a second file is a budget that drifts. src/core/jobs/tests/,
# src/core/reflect/tests/ and src/runtime/tests/ all do the same, for the same reason.
#
# THE SPLIT BETWEEN unit AND integration IS ABOUT COST, NOT SUBJECT. Everything this module does is
# arithmetic and finishes in microseconds; the one exception is `simulation-and-determinism`'s
# no-drift scenario, which names *hours* of ticks and is a quarter of a million calls. That case is
# in the integration suite and its shorter sibling stays in the unit one, with the reason written
# into test_scale.cpp's header. The measured numbers: the four-hour case is 12-15 ms in Debug and
# 1.5 ms in Development, against a unit budget of 1 ms in every profile.

set_property(GLOBAL PROPERTY CY_DETERMINISM_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_determinism_declare_tests)
    get_property(cy_determinism_tests_dir GLOBAL PROPERTY CY_DETERMINISM_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/core/determinism/tests: the '${kind}' entry of the test taxonomy could not be "
                "read from tests/CMakeLists.txt. If the budget variables were renamed, this lookup "
                "and the deferral around it should go away — declare the suites from tests/unit/ "
                "instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME determinism
        KIND unit
        SOURCES
            "${cy_determinism_tests_dir}/test_classification.cpp"
            "${cy_determinism_tests_dir}/test_clock.cpp"
            "${cy_determinism_tests_dir}/test_codec.cpp"
            "${cy_determinism_tests_dir}/test_commit.cpp"
            "${cy_determinism_tests_dir}/test_fp_policy.cpp"
            "${cy_determinism_tests_dir}/test_hash.cpp"
            "${cy_determinism_tests_dir}/test_profile.cpp"
            "${cy_determinism_tests_dir}/test_random.cpp"
            "${cy_determinism_tests_dir}/test_validator.cpp"
        DEPENDENCIES cy::core-determinism)

    cy_add_test(
        NAME determinism_scale
        KIND integration
        SOURCES
            "${cy_determinism_tests_dir}/test_chaos.cpp"
            "${cy_determinism_tests_dir}/test_scale.cpp"
        DEPENDENCIES cy::core-determinism)

    # THE UNIT SUITE IS COVERED BY THE PROFILE, AND THAT IS NOT DECORATION. test_profile.cpp's last
    # case asserts `BuildConfiguration::from_build().contraction_off`, which is true only because
    # cy_declare_determinism_profile() set CY_DETERMINISM_CONTRACTION_OFF on this target after
    # checking its compile options. Delete the flag below and the CONFIGURE fails; delete the
    # declaration and that case goes red. The seam between the two halves of task 2.2 is exactly
    # here.
    #
    # test_fp_policy.cpp needs it for a second reason: its ulp bounds are measured against `<cmath>`
    # with contraction off, and a fused multiply-add inside `fp::cbrt`'s Newton step would move the
    # last bit of the result the bounds are stated about.
    foreach(target cy_test_unit_determinism cy_test_integration_determinism_scale)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${target} PRIVATE -ffp-contract=off)
        elseif(MSVC)
            target_compile_options(${target} PRIVATE /fp:precise)
        endif()
        cy_declare_determinism_profile(${target}
            PROFILE SamePlatform
            REASON "it asserts what the profile's own build half reports, so it has to be covered by it")
    endforeach()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_determinism_declare_tests)

# --- The determinism lint, M9 task 2.6 ------------------------------------------------------------
#
# Registered with a bare add_test() rather than cy_add_test() for the reason src/core/config/tests/
# gives for `integration.project_graph`: it is not a C++ test binary and could not be one. Its two
# halves read files the BUILD wrote — `determinism-profiles.txt`, emitted by
# cy_declare_determinism_profile(), and `compile_commands.json`, emitted by CMake — and a C++
# program cannot see a compile line it was not compiled with.
#
# TWO ENTRIES, AND THE SECOND IS WHY THE FIRST MEANS ANYTHING.
#
#   determinism_lint           runs the real lint over every covered target and must exit zero.
#   determinism_lint_selftest  writes one fixture per rule and fails if the rule did not fire, plus
#                              a clean fixture that must produce nothing and an exemption that must
#                              apply to its own rule and to no other. Delete a rule from
#                              `scan_source()` and this goes red; without it, deleting a rule would
#                              make the first entry *greener*.
find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)

add_test(NAME integration.determinism_lint
    COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/../lint/determinism_lint.py"
            --root "${CMAKE_SOURCE_DIR}" --build "${CMAKE_BINARY_DIR}" --report-coverage)
set_tests_properties(integration.determinism_lint PROPERTIES TIMEOUT 300 LABELS integration)

add_test(NAME integration.determinism_lint_selftest
    COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/../lint/determinism_lint.py"
            --root "${CMAKE_SOURCE_DIR}" --selftest)
set_tests_properties(integration.determinism_lint_selftest PROPERTIES TIMEOUT 300
                     LABELS integration)
