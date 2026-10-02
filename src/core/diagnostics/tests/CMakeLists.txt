# src/core/diagnostics/tests/ — the diagnostics module's suites, and the checks that are not C++.
#
# THE C++ SUITES are declared with cy_add_test(), which tests/CMakeLists.txt defines — and the
# top-level CMakeLists.txt adds tests/ *after* src/, so that function does not exist while this file
# is read. The declaration is deferred to the end of the top-level directory's processing, the way
# src/core/serialize/tests/ and every other module with its own suites do it; the taxonomy is read
# back out of tests/ rather than restated.
#
# Until issue #90 they were executables with a three-macro harness and a main() each, registered
# with plain add_test(), because they were written before cy/test/test.h existed. That made every
# check in them uncitable: tools/roadmap/requirements.py resolves `test:` evidence only against
# suites cy_add_test declares, by case name. They are now one suite per file, one case per
# behaviour, and the CTest names are `integration.diagnostics_<subject>`.
#
# Every suite is `integration`, not `unit`, deliberately: each opens a trace, writes an artefact and
# reads it back, and the capture suite starts threads — subsystems together and I/O, which is what
# `testing-and-quality` means by the word. The unit budget is a millisecond and none of these would
# honestly fit it.
#
# Each suite is its own binary rather than one binary of nine sources, and that is the isolation
# the executables had: the trace, the registry, the health model and the log floors are process
# state, and a case that reads "no registered name is a source path" reads every name this process
# ever registered.
#
# enable_testing() is called in the top-level CMakeLists; do not call it again here.

set_property(GLOBAL PROPERTY CY_CORE_DIAGNOSTICS_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set_property(GLOBAL PROPERTY CY_CORE_DIAGNOSTICS_TESTS_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")

function(cy_core_diagnostics_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_CORE_DIAGNOSTICS_TESTS_DIR)
    get_property(binary_dir GLOBAL PROPERTY CY_CORE_DIAGNOSTICS_TESTS_BINARY_DIR)

    # cy_add_test() reads the taxonomy from variables that tests/CMakeLists.txt sets in its own
    # directory scope, and this function runs in the top-level scope, which is not a child of it.
    get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_integration)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_integration)
    if(NOT budget OR NOT timeout)
        message(FATAL_ERROR
            "src/core/diagnostics/tests: the 'integration' entry of the test taxonomy could not be "
            "read from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
            "the deferral around it should go away.")
    endif()
    set(CY_TEST_BUDGET_integration "${budget}")
    set(CY_TEST_TIMEOUT_integration "${timeout}")

    #   diagnostics_trace           identity, the one timeline, offline name resolution, and
    #                               emission that allocates nothing
    #   diagnostics_loss            drops by priority, a flood on every channel, the loss recorded
    #   diagnostics_log             level and category floors, sites, a query by typed field
    #   diagnostics_capture         the rolling buffer, its triggers, the health model, teardown
    #   diagnostics_privacy         export policies, redaction, unclassified fields
    #   diagnostics_source_privacy  a source location as classified data, no build-machine path
    #   diagnostics_bridge          base's diagnostic sink as records on the timeline
    #   diagnostics_reproduction    the reproduction artefact and the crash report's link to it
    #   diagnostics_overhead        the engine's report of its own emission cost
    cy_add_test(
        NAME diagnostics_trace
        KIND integration
        SOURCES "${tests_dir}/test_trace.cpp"
        DEPENDENCIES cy::core-diagnostics)

    cy_add_test(
        NAME diagnostics_loss
        KIND integration
        SOURCES "${tests_dir}/test_loss.cpp"
        DEPENDENCIES cy::core-diagnostics)

    cy_add_test(
        NAME diagnostics_log
        KIND integration
        SOURCES "${tests_dir}/test_log.cpp"
        DEPENDENCIES cy::core-diagnostics)

    cy_add_test(
        NAME diagnostics_capture
        KIND integration
        SOURCES "${tests_dir}/test_capture.cpp"
        DEPENDENCIES cy::core-diagnostics)

    cy_add_test(
        NAME diagnostics_privacy
        KIND integration
        SOURCES "${tests_dir}/test_privacy.cpp"
        DEPENDENCIES cy::core-diagnostics)

    cy_add_test(
        NAME diagnostics_source_privacy
        KIND integration
        SOURCES "${tests_dir}/test_source_privacy.cpp"
        DEPENDENCIES cy::core-diagnostics)

    cy_add_test(
        NAME diagnostics_bridge
        KIND integration
        SOURCES "${tests_dir}/test_bridge.cpp"
        DEPENDENCIES cy::core-diagnostics)

    cy_add_test(
        NAME diagnostics_reproduction
        KIND integration
        SOURCES "${tests_dir}/test_reproduction.cpp"
        DEPENDENCIES cy::core-diagnostics)

    cy_add_test(
        NAME diagnostics_overhead
        KIND integration
        SOURCES "${tests_dir}/test_overhead.cpp"
        DEPENDENCIES cy::core-diagnostics)

    # The suites write their artefacts by relative name, as the executables did, so they run where
    # the executables ran — this module's build directory — rather than in the top of the build
    # tree, which is where a deferred declaration would otherwise put them.
    foreach(suite IN ITEMS trace loss log capture privacy source_privacy bridge reproduction
                           overhead)
        set_tests_properties(integration.diagnostics_${suite} PROPERTIES
            WORKING_DIRECTORY "${binary_dir}")
    endforeach()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_core_diagnostics_declare_tests)

# --- What is not C++ ----------------------------------------------------------------------------
#
# The crash probe and the four Python checks stay plain add_test() entries: a probe that must
# really die cannot be a case in a suite that has to survive it, and the others are a compiler run,
# two lint runs and a reader. They carry the `integration` label like everything else here, and the
# `diagnostics-privacy` gate in tools/roadmap/gates.toml runs them by name beside the two privacy
# suites, which is what makes "the check SHALL be a gate rather than a review" true of them.

# A program that really crashes, and a test that reads the artefact it leaves behind.
cy_add_module(
    NAME cy_diag_crash_probe
    LAYER core
    TYPE EXECUTABLE
    SOURCES crash_probe.cpp
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)

# `--inspect` is the READER, and it is here because M10 changed the artefact's frame format to close
# `m9:crash-artefact-paths` and found that `tools/trace/crash_inspect.py` — the whole of
# `just diagnose-crash` — was reachable from no test at all. A format nothing reads back is a format
# that breaks silently. The path is compiled in rather than guessed relative to this file, for the
# reason tests/integration's seam test gives: a test that guesses where a tool is passes vacuously
# when it guesses wrong.
add_test(NAME diagnostics.crash
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_crash.py"
            --probe $<TARGET_FILE:cy_diag_crash_probe>
            --inspect "${CMAKE_SOURCE_DIR}/tools/trace/crash_inspect.py")
set_tests_properties(diagnostics.crash PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" TIMEOUT 120 LABELS integration)

# The class, not the instance. M0's gate found `__FILE__` reaching the trace's NAME table and
# repaired it with `-fmacro-prefix-map`; a flag repairs the instance, and this lint is what stops the
# shape coming back anywhere in the tree. It scans src/, samples/ and tools/ rather than this module,
# because the forbidden call is available to every subsystem that includes field.h.
#
# --self-test runs it over a snippet that contains the shape and fails if it is not found, so the
# gate cannot pass by having stopped working.
add_test(NAME diagnostics.source_location_lint_self_test
    COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/../lint/lint_source_locations.py" --self-test)
# `integration`, not `unit`: it starts a Python interpreter, and the unit tier is sub-millisecond
# with no I/O. A process spawn in the fast set is a budget nobody meant to spend.
set_tests_properties(diagnostics.source_location_lint_self_test PROPERTIES
    TIMEOUT 60 LABELS integration)

add_test(NAME diagnostics.source_location_lint
    COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/../lint/lint_source_locations.py"
            "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/samples" "${CMAKE_SOURCE_DIR}/tools")
set_tests_properties(diagnostics.source_location_lint PROPERTIES
    TIMEOUT 120 LABELS integration)

# The compile-time half of the privacy invariant: three declarations that must not compile.
add_test(NAME diagnostics.field_macro
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_field_macro.py"
            --compiler "${CMAKE_CXX_COMPILER}"
            --include "${CMAKE_CURRENT_SOURCE_DIR}/../include"
            --include "${CMAKE_CURRENT_SOURCE_DIR}/../../base/include"
            --sources "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail")
set_tests_properties(diagnostics.field_macro PROPERTIES TIMEOUT 120 LABELS integration)

# The overhead benchmark. A program rather than a suite: it prints a table of figures at a sample
# count chosen by whoever runs it, and asserts nothing about them. `just diagnose-overhead` builds
# and runs it; integration.diagnostics_overhead is the suite that holds what it relies on.
cy_add_module(
    NAME cy_diag_overhead
    LAYER core
    TYPE EXECUTABLE
    SOURCES ../bench/diag_overhead.cpp
    PRIVATE_DEPENDENCIES cy::core-diagnostics)
