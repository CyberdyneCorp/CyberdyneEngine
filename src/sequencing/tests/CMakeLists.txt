# src/sequencing/tests/ — the suites for CyberSequence. M8.c section 3.
#
# TWO KINDS, and the division is `testing-and-quality`'s budget rather than taste. `unit` holds the
# arithmetic and the compiler: exact time, the interval and event indexes, channel evaluation,
# compression, and every refusal the specification requires — none of which builds a world, a device
# or a camera server. `integration` holds playback, the two bridges and the scale case: a player
# driving a camera server, a command stream, or a million keys is not a one-millisecond test, and
# putting it in `unit` would make the suite red on a loaded machine rather than red on a defect.
set_property(GLOBAL PROPERTY CY_SEQUENCING_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_sequencing_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_SEQUENCING_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/sequencing/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME sequencing
        KIND unit
        SOURCES
            "${tests_dir}/test_time.cpp"
            "${tests_dir}/test_compile.cpp"
            "${tests_dir}/test_program.cpp"
            "${tests_dir}/test_dispatch.cpp"
        DEPENDENCIES cy::sequencing)

    cy_add_test(
        NAME sequencing_playback
        KIND integration
        SOURCES
            "${tests_dir}/test_playback.cpp"
            "${tests_dir}/test_scale.cpp"
        DEPENDENCIES cy::sequencing)

    cy_add_test(
        NAME sequencing_camera
        KIND integration
        SOURCES
            "${tests_dir}/test_camera_bridge.cpp"
        DEPENDENCIES cy::sequencing cy::sequencing-camera cy::servers-camera)

    cy_add_test(
        NAME sequencing_commands
        KIND integration
        SOURCES
            "${tests_dir}/test_commands.cpp"
        DEPENDENCIES cy::sequencing cy::sequencing-gameplay cy::gameplay)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_sequencing_declare_tests)
