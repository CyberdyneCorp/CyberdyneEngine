# bindings/swift/ — the CyberdyneKit Swift package's place in the engine's build. Section 3.
#
# --- WHAT THIS FILE DOES AND, MORE IMPORTANTLY, WHAT IT DOES NOT ---------------------------------
#
# It does NOT build the Swift package. The Swift package manager owns that, and the top-level
# CMakeLists.txt says so in as many words: "Cargo owns the first and the Swift package manager the
# second, and the C++ build does not reimplement either." Reimplementing it would also break
# `swift-scripting`'s "Standard Swift tooling" scenario — `swift build`, `swift test`, Xcode and
# SourceKit-LSP have to work on the package as it stands, with no custom toolchain.
#
# What it does is REGISTER THE CHECKS, so that they run under `just test-all` rather than only when
# somebody remembers a second command. `testing-and-quality`'s position throughout this tree is that
# a check nobody runs has silently stopped firing, and four of the five here are exactly the kind
# that would:
#
#   integration.swift_overlay        the committed overlay is what regeneration produces
#   integration.swift_overlay_gen    the generator's own selftest
#   integration.swift_package        `swift test` — the package's own cases
#   integration.swift_no_runtime     task 3.9: the engine core links no Swift runtime
#   integration.swift_reload         a real Swift module, hot-reloaded by the engine's loader
#
# --- WHY THE SUITES ARE ABSENT RATHER THAN SKIPPING WITHOUT A TOOLCHAIN ---------------------------
#
# The same arrangement tests/render/ uses for a machine with no GPU, and for the same reason. A
# suite that is registered and skips is a suite whose green means nothing; a suite that is not
# registered is visible as absent at configure time, where the message says why. `just env-doctor`
# is the place that reports a missing Swift toolchain as a problem, and it already knows how to
# diagnose the `~/.profile` case specifically.

find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "Swift bindings: no Python 3 interpreter; the overlay checks are not registered")
    return()
endif()

set(CY_SWIFT_DRIVER "${CMAKE_CURRENT_SOURCE_DIR}/tools/cy_swift_module.py")
set(CY_SWIFT_PACKAGE "${CMAKE_CURRENT_SOURCE_DIR}")

# --- The two checks that need no Swift toolchain --------------------------------------------------
#
# The overlay is COMMITTED, which is what `swift-scripting` requires — "Generated code SHALL be
# committed to the repository so consumers do not need the generator" — so its currency check is a
# diff over the source tree and runs everywhere, including on a machine that cannot build Swift at
# all. That is the point: a stale overlay is caught by whoever builds the engine, not only by
# whoever builds Swift.

add_test(NAME integration.swift_overlay
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen/swift/overlay_gen.py"
                 --check --package "${CY_SWIFT_PACKAGE}"
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(integration.swift_overlay PROPERTIES LABELS "integration" TIMEOUT 300)

add_test(NAME integration.swift_overlay_gen
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen/swift/tests/run_tests.py"
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(integration.swift_overlay_gen PROPERTIES LABELS "integration" TIMEOUT 300)

# --- Task 3.9: the engine core links no Swift runtime ----------------------------------------------
#
# Registered unconditionally, and deliberately so. It is a statement about the ENGINE's binaries, so
# it must hold on a machine with no Swift toolchain — indeed it is most interesting there, because
# that is the configuration in which nobody would notice a Swift dependency until a user did.
add_test(NAME integration.swift_no_runtime
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_no_swift_runtime.py"
                 --build-dir "${CMAKE_BINARY_DIR}")
set_tests_properties(integration.swift_no_runtime PROPERTIES LABELS "integration" TIMEOUT 300)

# --- Everything past here needs the Swift toolchain -------------------------------------------------

execute_process(COMMAND "${Python3_EXECUTABLE}" "${CY_SWIFT_DRIVER}" --probe
                RESULT_VARIABLE swift_probe_result
                OUTPUT_VARIABLE swift_probe_output
                ERROR_VARIABLE swift_probe_error
                OUTPUT_STRIP_TRAILING_WHITESPACE)

if(NOT swift_probe_result EQUAL 0)
    message(STATUS
        "Swift bindings: no Swift toolchain, so integration.swift_package and "
        "integration.swift_reload are not registered.\n"
        "   `just env-doctor` diagnoses this, including the case where swiftly wrote its "
        "environment line to ~/.profile and a non-login shell never reads it.")
    return()
endif()

message(STATUS "Swift bindings: ${swift_probe_output}")

# `swift test` over the package. An `add_test` rather than a `cy_add_test`, because there is no C++
# translation unit here to compile a per-case budget into: the taxonomy's budget is enforced inside
# the binary by cy::test::BudgetGuard, and this binary is the Swift test runner. It carries the
# integration label and the integration timeout, which is what the recipes select on.
add_test(NAME integration.swift_package
         COMMAND "${Python3_EXECUTABLE}" "${CY_SWIFT_DRIVER}" --test
         WORKING_DIRECTORY "${CY_SWIFT_PACKAGE}")
set_tests_properties(integration.swift_package PROPERTIES LABELS "integration" TIMEOUT 900)

# --- The reload fixture: three generations of one Swift module -------------------------------------
#
# Built at BUILD time rather than inside the test, for the reason the taxonomy gives: an integration
# case has a one-second CPU budget and compiling Swift does not fit in it. src/abi/tests/ builds its
# C modules the same way and for the same reason.
#
# Three generations from two source directories: 0 and 2 are the same source (schema 1), so that
# reloading 1 -> 2 is a DOWNGRADE and exercises the schema check from the Swift side. Each gets its
# own file AND its own Swift `-module-name`, which the spike measured to be necessary and which the
# loader cannot check for itself.
# The Swift configuration follows the engine's, so that `just test-all --profile release` exercises
# `swiftc -O` and not only `-Onone`. M4's hot-reload spike ran its whole matrix at both and diffed
# the verdicts — they were identical — and the only way that stays true is if both are still built.
if(CMAKE_BUILD_TYPE STREQUAL "Profile" OR CMAKE_BUILD_TYPE STREQUAL "Shipping")
    set(CY_SWIFT_CONFIGURATION release)
else()
    set(CY_SWIFT_CONFIGURATION debug)
endif()

set(CY_SWIFT_MODULE_DIR "${CMAKE_CURRENT_BINARY_DIR}/modules")
set(CY_SWIFT_MODULE_WORK "${CMAKE_CURRENT_BINARY_DIR}/module-build")
set(CY_SWIFT_MODULE_G0 "${CY_SWIFT_MODULE_DIR}/libCyGame_g0.so")
set(CY_SWIFT_MODULE_G1 "${CY_SWIFT_MODULE_DIR}/libCyGame_g1.so")
set(CY_SWIFT_MODULE_G2 "${CY_SWIFT_MODULE_DIR}/libCyGame_g2.so")

# A GLOB, AND THE ONE PLACE THE RULE AGAINST THEM DOES NOT APPLY. `build-system-and-platforms`
# forbids globbed *target sources*, because a file that silently joins or leaves a compilation is a
# build nobody can read. This is not a target's source list: SwiftPM decides what to compile, and
# these two lists are only the DEPENDS of the custom command that invokes it — a change to any Swift
# file must re-run the driver, and a list written by hand would go stale the first time somebody
# added a file to CyberdyneKit and wondered why the fixture was still last week's. src/core/reflect/
# globs the reflection generator's own sources for exactly this reason and says so.
file(GLOB CY_SWIFT_FIXTURE_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/reload/v1/*.swift"
     "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/reload/v2/*.swift")
file(GLOB_RECURSE CY_SWIFT_PACKAGE_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/Sources/*.swift")

add_custom_command(
    OUTPUT "${CY_SWIFT_MODULE_G0}" "${CY_SWIFT_MODULE_G1}" "${CY_SWIFT_MODULE_G2}"
    COMMAND "${Python3_EXECUTABLE}" "${CY_SWIFT_DRIVER}"
            --work "${CY_SWIFT_MODULE_WORK}" --out "${CY_SWIFT_MODULE_DIR}"
            --configuration "${CY_SWIFT_CONFIGURATION}"
            --generation "0=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/reload/v1"
            --generation "1=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/reload/v2"
            --generation "2=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/reload/v1"
    DEPENDS "${CY_SWIFT_DRIVER}" "${CMAKE_CURRENT_SOURCE_DIR}/Package.swift"
            ${CY_SWIFT_FIXTURE_SOURCES} ${CY_SWIFT_PACKAGE_SOURCES}
    COMMENT "Building three generations of the Swift reload fixture"
    VERBATIM)

add_custom_target(cy_swift_reload_fixture ALL
                  DEPENDS "${CY_SWIFT_MODULE_G0}" "${CY_SWIFT_MODULE_G1}" "${CY_SWIFT_MODULE_G2}")

if(CY_BUILD_TESTS)
    # The taxonomy's budget and timeout live in tests/CMakeLists.txt and are read back out of it
    # rather than restated here — the same thing src/abi/tests/ does, for the same reason: a budget
    # copied into a second file drifts. This directory is added after tests/, so the values are
    # already there and no deferral is needed.
    get_directory_property(cy_swift_budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_integration)
    get_directory_property(cy_swift_timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_integration)
    if(NOT cy_swift_budget OR NOT cy_swift_timeout)
        message(FATAL_ERROR
            "bindings/swift: the 'integration' entry of the test taxonomy could not be read from "
            "tests/CMakeLists.txt. If the budget variables were renamed, declare this suite from "
            "tests/integration/ instead.")
    endif()
    set(CY_TEST_BUDGET_integration "${cy_swift_budget}")
    set(CY_TEST_TIMEOUT_integration "${cy_swift_timeout}")

    cy_add_test(
        NAME swift_reload
        KIND integration
        SOURCES tests/test_swift_reload.cpp
        DEPENDENCIES cy::abi
        DEFINITIONS
            "CY_SWIFT_MODULE_G0=\"${CY_SWIFT_MODULE_G0}\""
            "CY_SWIFT_MODULE_G1=\"${CY_SWIFT_MODULE_G1}\""
            "CY_SWIFT_MODULE_G2=\"${CY_SWIFT_MODULE_G2}\"")
    add_dependencies(cy_test_integration_swift_reload cy_swift_reload_fixture)
endif()
