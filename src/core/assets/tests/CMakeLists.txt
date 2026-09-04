# src/core/assets/tests/ — the asset layer's suites. Section 3.3.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is therefore deferred: this file defines one function and asks CMake to call it at the end of the
# top-level directory's processing, by which time the taxonomy has been declared. (A deferred
# add_subdirectory() is rejected outright by CMake — "Subdirectories may not be created during
# deferred execution" — so the deferral has to be of the declaration, not of the directory.)
#
# THIS IS THE FOURTH MODULE TO NEED THIS WORKAROUND: src/core/reflect/, src/core/values/ and
# src/core/jobs/ each hit it independently and each solved it the same way. Whoever closes M1 should
# settle it properly — either move `add_subdirectory(tests)` above `add_subdirectory(src)` in the
# top-level CMakeLists, or lift the taxonomy into a cmake/ module that both sides include.
#
# THE SPLIT BETWEEN unit AND integration IS ABOUT COST, NOT SUBJECT. A unit test has a millisecond
# and does no I/O and starts no thread. `assets` is paths, identity and the two serialization forms:
# in-memory, and fast. `assets_io` writes real files, `assets_codec` compresses hundreds of
# kilobytes, and `assets_loading` starts a job system and an I/O thread — all three are integration.

# This directory's path travels to the deferred call in a global property. Neither an ordinary
# variable nor a deferred-call argument survives the trip: the call runs in the top-level scope,
# which is not a child of this one, and a global property is the one place both scopes can see.
set_property(GLOBAL PROPERTY CY_ASSETS_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_assets_declare_tests)
    get_property(cy_assets_tests_dir GLOBAL PROPERTY CY_ASSETS_TESTS_DIR)

    # cy_add_test() reads the taxonomy from variables that tests/CMakeLists.txt sets in its own
    # directory scope, and this function runs in the top-level scope, which is not a child of it.
    # They are read back from the directory that defines them rather than restated here: a budget
    # copied into a second file is a budget that drifts.
    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/core/assets/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
                "the deferral around it should go away — declare the suites directly from "
                "tests/unit/assets/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    # Sources are absolute, because the deferred call runs in the top-level directory's scope and a
    # relative path would be resolved against that directory rather than against this one.
    cy_add_test(
        NAME assets
        KIND unit
        SOURCES
            "${cy_assets_tests_dir}/test_identity.cpp"
            "${cy_assets_tests_dir}/test_path.cpp"
            "${cy_assets_tests_dir}/test_serialization.cpp"
        DEPENDENCIES cy::core-assets cy::core-reflect)

    cy_add_test(
        NAME assets_codec
        KIND integration
        SOURCES "${cy_assets_tests_dir}/test_compression.cpp"
        DEPENDENCIES cy::core-assets)

    cy_add_test(
        NAME assets_io
        KIND integration
        SOURCES
            "${cy_assets_tests_dir}/test_file.cpp"
            "${cy_assets_tests_dir}/test_package.cpp"
            "${cy_assets_tests_dir}/test_vfs.cpp"
        DEPENDENCIES cy::core-assets)

    cy_add_test(
        NAME assets_loading
        KIND integration
        SOURCES "${cy_assets_tests_dir}/test_asset_system.cpp"
        DEPENDENCIES cy::core-assets cy::core-jobs)

    # The watcher and the reload it feeds. Integration rather than unit for the same reason
    # `assets_loading` is: the reload cases start a job system and an I/O thread. The watcher's own
    # cases would fit a millisecond, and they are here rather than split across two suites because
    # the two halves of hot reload are one subject and a failure in either is read together.
    cy_add_test(
        NAME assets_watch
        KIND integration
        SOURCES "${cy_assets_tests_dir}/test_watch.cpp"
        DEPENDENCIES cy::core-assets cy::core-jobs)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_assets_declare_tests)
