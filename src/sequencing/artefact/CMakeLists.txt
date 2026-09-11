# src/sequencing/artefact/ — the capture of a cut, mid-blend. M8.c task 5.5's camera half.
#
# WHY THERE IS A PROGRAM HERE AND NOT ONLY A TEST. A blend is a thing a person LOOKS at. A test can
# assert that the camera stack reported two contributions at 0.5 and 0.5 and be green with the
# camera pointing at the floor; the milestone's rule 14 asks for the picture. So this target plays a
# real sequence through a real `CameraServer`, and prints — per frame — the stack's own contribution
# report and the view-projection matrix the blended camera produced. `cut.py` turns that into a PNG
# by projecting the scene's points through those matrices, which means the picture is drawn FROM the
# camera the stack produced: if the blend were wrong, the picture would be wrong.
#
# It reports through samples/harness/artefact.py, so a gap it records cannot exit zero.
cy_add_module(
    NAME cy_sequence_cut-capture
    LAYER tools
    TYPE EXECUTABLE
    SOURCES
        cut.cpp
    PRIVATE_DEPENDENCIES
        cy::sequencing
        cy::sequencing-camera
        cy::sequencing-gameplay
        cy::servers-camera)

if(NOT CY_BUILD_TESTS)
    return()
endif()

find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "src/sequencing/artefact: python3 is absent, so smoke.sequence_cut is not "
                   "declared.")
    return()
endif()

# DEFERRED to the end of the top-level directory, exactly as src/sequencing/tests/ is, and for the
# same reason: tests/CMakeLists.txt is read AFTER src/, so the taxonomy's timeouts do not exist yet
# while this file is being processed. Reading them early gave "requested directory not found",
# which names the lookup rather than the ordering — hence this comment.
set_property(GLOBAL PROPERTY CY_SEQUENCE_ARTEFACT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set_property(GLOBAL PROPERTY CY_SEQUENCE_ARTEFACT_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set_property(GLOBAL PROPERTY CY_SEQUENCE_ARTEFACT_PYTHON "${Python3_EXECUTABLE}")

function(cy_sequence_declare_artefact)
    get_property(artefact_dir GLOBAL PROPERTY CY_SEQUENCE_ARTEFACT_DIR)
    get_property(binary_dir GLOBAL PROPERTY CY_SEQUENCE_ARTEFACT_BINARY_DIR)
    get_property(python GLOBAL PROPERTY CY_SEQUENCE_ARTEFACT_PYTHON)
    get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_smoke)
    if(NOT timeout)
        message(FATAL_ERROR
            "src/sequencing/artefact: the test taxonomy's timeouts could not be read from "
            "tests/CMakeLists.txt. If those variables were renamed, this lookup should move "
            "with them.")
    endif()

    add_test(NAME smoke.sequence_cut
             COMMAND "${python}" "${artefact_dir}/cut.py"
                     --program "$<TARGET_FILE:cy_sequence_cut-capture>"
                     --work "${binary_dir}/run"
             WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(smoke.sequence_cut PROPERTIES LABELS "smoke" TIMEOUT "${timeout}")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_sequence_declare_artefact)
