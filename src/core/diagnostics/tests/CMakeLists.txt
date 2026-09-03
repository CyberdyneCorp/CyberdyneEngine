# The diagnostics tests, as their own executables.
#
# They belong in tests/unit/diagnostics/ under the CY_TEST_CASE wrapper of task 4.1.1; that wrapper
# did not exist when this module was written, and defining a second one here would be the collision
# it exists to prevent. Each executable is registered with add_test(), so `ctest` runs them today and
# moving them later changes their registration, not their content.
#
# enable_testing() is called in the top-level CMakeLists; do not call it again here.
#
# Every test here carries the `integration` label, so `just test-integration` and the integration
# pass of `just test-all` select them and no test of this module falls into the unclassified pass.
# Integration rather than unit deliberately: each opens a trace, writes an artefact and reads it
# back, and two of them start a process — that is subsystems together and it is I/O, which is what
# `testing-and-quality` means by the word. The unit budget is a millisecond and none of these would
# honestly fit it.

set(cy_diagnostics_tests test_trace test_privacy test_loss test_log test_bridge)

foreach(cy_test IN LISTS cy_diagnostics_tests)
    cy_add_module(
        NAME cy_diag_${cy_test}
        LAYER core
        TYPE EXECUTABLE
        SOURCES ${cy_test}.cpp
        PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        PRIVATE_DEPENDENCIES cy::core-diagnostics)
    add_test(NAME diagnostics.${cy_test} COMMAND cy_diag_${cy_test})
    set_tests_properties(diagnostics.${cy_test} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" TIMEOUT 60 LABELS integration)
endforeach()

# A program that really crashes, and a test that reads the artefact it leaves behind.
cy_add_module(
    NAME cy_diag_crash_probe
    LAYER core
    TYPE EXECUTABLE
    SOURCES crash_probe.cpp
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)

add_test(NAME diagnostics.crash
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_crash.py"
            --probe $<TARGET_FILE:cy_diag_crash_probe>)
set_tests_properties(diagnostics.crash PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" TIMEOUT 120 LABELS integration)

# The compile-time half of the privacy invariant: three declarations that must not compile.
add_test(NAME diagnostics.field_macro
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_field_macro.py"
            --compiler "${CMAKE_CXX_COMPILER}"
            --include "${CMAKE_CURRENT_SOURCE_DIR}/../include"
            --include "${CMAKE_CURRENT_SOURCE_DIR}/../../base/include"
            --sources "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail")
set_tests_properties(diagnostics.field_macro PROPERTIES TIMEOUT 120 LABELS integration)

# The overhead measurement. Registered as a test with a small sample count so that a regression that
# makes emission allocate is caught; the real numbers come from `just diagnose-overhead`.
cy_add_module(
    NAME cy_diag_overhead
    LAYER core
    TYPE EXECUTABLE
    SOURCES ../bench/diag_overhead.cpp
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

add_test(NAME diagnostics.overhead COMMAND cy_diag_overhead 20000)
set_tests_properties(diagnostics.overhead PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" TIMEOUT 120 LABELS integration)
