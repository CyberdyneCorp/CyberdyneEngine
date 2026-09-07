# samples/harness — what every artefact in this directory reports through. M7 tasks 5b.5 and 5b.5b.
#
# --- WHY THIS IS A DIRECTORY AND NOT A FUNCTION IN EACH ARTEFACT ----------------------------------
#
# Because the two rules it carries are rules about artefacts in general, and both were broken at M6
# by an artefact that was otherwise careful:
#
#   * `samples/05b-editor-window/window.py` printed `GAP the gizmo drag` and returned 0, so
#     `smoke.editor_window` was green while the milestone's headline interaction was unproven.
#   * `samples/06-open-world/openworld.py` led with `worst tick 358 us`; four re-runs on a verified
#     empty machine gave 365.9 to 392.2, every one of them worse, while the median moved 0.6 us.
#
# Neither is a defect in the subject either artefact was about, and neither would have been caught
# by a reviewer reading that artefact — they are defects in how a run turns into a status and into a
# headline. So the status is DERIVED from the recorded gaps in `artefact.Report.exit_code`, and the
# headline is REFUSED for an extreme-value statistic in `artefact.Report.headline`, and a later
# artefact cannot repeat either without deleting this module.
#
# --- WHY IT DECLARES A TEST -----------------------------------------------------------------------
#
# The harness is the one thing in samples/ whose failure is invisible: if `exit_code` stopped
# counting gaps, every artefact would go green at once and nothing else would change. `selftest.py`
# is the check, and it needs no display, no device and no engine, so it runs everywhere the unit
# suite runs.

if(NOT CY_BUILD_TESTS)
    return()
endif()

find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "samples/harness: python3 was not found; unit.artefact_harness is not registered")
    return()
endif()

get_directory_property(CY_HARNESS_TIMEOUT DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                       DEFINITION CY_TEST_TIMEOUT_unit)
if(NOT CY_HARNESS_TIMEOUT)
    message(FATAL_ERROR
        "samples/harness: the test taxonomy's timeouts could not be read from tests/CMakeLists.txt.")
endif()

add_test(NAME unit.artefact_harness
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/selftest.py"
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(unit.artefact_harness PROPERTIES
                     LABELS "unit" TIMEOUT "${CY_HARNESS_TIMEOUT}")
