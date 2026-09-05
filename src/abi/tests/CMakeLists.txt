# src/abi/tests/ — the ABI's suites, and the C modules the reload suite loads. Section 2.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing and the taxonomy is read back out of
# tests/ rather than restated, exactly as src/ecs/, src/scene/, src/servers/render/ and
# src/rendering/graph/ do it, and for the same reason: a budget copied into a second file drifts.
#
#   unit.abi          the table, the layout, the value marshalling, the manifest. No file system, no
#                     dynamic loader, no device — every case is arithmetic over an in-process world.
#   integration.abi_reload
#                     the reload sequence, which opens real shared libraries from disk. It is an
#                     integration test because dlopen is, not because it is slow: the whole suite is
#                     a few milliseconds.
#
# --- THE FOUR TEST MODULES, AND WHY THEY ARE C ---------------------------------------------------
#
# One source file, built four ways. They are the other side of the boundary, and their being C is
# what makes "the ABI consists only of C constructs" a build failure rather than a claim: cy_abi.h is
# included from C++ everywhere else in the tree, and C++ accepts a great deal that C does not.
#
# They link NOTHING of the engine — only the include path. Linking cy::abi would give each module its
# own copy of `cy_get_interface` and of the host's state, which is precisely the mistake the
# single-exported-symbol rule exists to prevent, and it would make the reload suite pass against four
# private engines.
#
# `native-abi` also requires that a module built with a different compiler load correctly. That is
# not asserted here, because this machine has one toolchain; what is asserted is the property that
# makes it possible — the header is C, the structs are fixed-width and explicitly padded, and every
# size and offset is checked against the description generator in unit.abi.

set_property(GLOBAL PROPERTY CY_ABI_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# One build of src/abi/tests/module/behaviour.c.
function(cy_abi_test_module name)
    cy_add_module(
        NAME ${name}
        LAYER tools
        TYPE SHARED
        SOURCES module/behaviour.c
        PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../include"
        PRIVATE_DEFINITIONS ${ARGN})
endfunction()

# schema 1: health, ammo, ticks.
cy_abi_test_module(cy_abi_test_module_v1 CY_TEST_MODULE_SCHEMA=1)
# schema 2: ammo becomes mana (halved), shield is new and defaults. A DIFFERENT LAYOUT, which is the
# case in-place preservation gets wrong silently.
cy_abi_test_module(cy_abi_test_module_v2 CY_TEST_MODULE_SCHEMA=2)
# The entry point that returns false: reported by the loader, fatal to nothing.
cy_abi_test_module(cy_abi_test_module_refuse CY_TEST_MODULE_SCHEMA=1 CY_TEST_MODULE_REFUSE=1)
# schema 2 under another type name, so a live instance has nowhere to go.
cy_abi_test_module(cy_abi_test_module_renamed CY_TEST_MODULE_SCHEMA=2 CY_TEST_MODULE_RENAMED=1)

function(cy_abi_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_ABI_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/abi/tests: the '${kind}' entry of the test taxonomy could not be read from "
                "tests/CMakeLists.txt. If the budget variables were renamed, this lookup and the "
                "deferral around it should go away — declare the suites from tests/unit/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME abi
        KIND unit
        SOURCES
            "${tests_dir}/test_interface.cpp"
            "${tests_dir}/test_layout.cpp"
            "${tests_dir}/test_manifest.cpp"
        DEPENDENCIES cy::abi)

    # The module paths are compiled in rather than searched for at run time: a test that guessed a
    # path would pass on the machine it was written on and skip everywhere else, and a reload suite
    # that silently skips is the worst of the available outcomes.
    cy_add_test(
        NAME abi_reload
        KIND integration
        SOURCES "${tests_dir}/test_reload.cpp"
        DEPENDENCIES cy::abi
        DEFINITIONS
            "CY_ABI_TEST_MODULE_V1=\"$<TARGET_FILE:cy_abi_test_module_v1>\""
            "CY_ABI_TEST_MODULE_V2=\"$<TARGET_FILE:cy_abi_test_module_v2>\""
            "CY_ABI_TEST_MODULE_REFUSE=\"$<TARGET_FILE:cy_abi_test_module_refuse>\""
            "CY_ABI_TEST_MODULE_RENAMED=\"$<TARGET_FILE:cy_abi_test_module_renamed>\"")

    # The suite opens those libraries by path, so they must exist before it runs. Nothing links them,
    # so nothing else would order them.
    add_dependencies(cy_test_integration_abi_reload
                     cy_abi_test_module_v1 cy_abi_test_module_v2
                     cy_abi_test_module_refuse cy_abi_test_module_renamed)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_abi_declare_tests)

# --- The compatibility gate, as a test ------------------------------------------------------------
#
# Task 2.8: "ABI compatibility testing in CI". The gate is a Python program over a committed
# baseline, so it cannot be a C++ test — a program that links is a program the ABI already accepted.
# It is registered here, the way src/core/config/tests/ registers the project graph's rejections and
# for the same reason: a check that runs only when somebody remembers the recipe is a check that has
# silently stopped firing.
#
# Two tests, because they answer different questions:
#
#   integration.abi_baseline  the header in the tree matches src/abi/abi_baseline.json. This is the
#                             gate itself, and it is what fails when somebody reorders an entry.
#   integration.abi_gate      the gate still REFUSES a reorder, a removal, a signature change, an
#                             inserted struct member and a changed enum value, and still ACCEPTS an
#                             append. design.md §1: a gate that has never failed is a gate nobody
#                             knows works.
find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)

add_test(NAME integration.abi_baseline
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/abi/abi_gate.py")
set_tests_properties(integration.abi_baseline PROPERTIES TIMEOUT 300 LABELS integration)

add_test(NAME integration.abi_gate
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/abi/selftest.py")
set_tests_properties(integration.abi_gate PROPERTIES TIMEOUT 300 LABELS integration)
