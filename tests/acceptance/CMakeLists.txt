# tests/acceptance/ — the acceptance scenarios `testing-and-quality` names. M11.d task 7.2.
#
# "The suite SHALL include acceptance scenarios exercising the architecture rather than a favourable
# case": STRATEGY STRESS, CONTROL HANDOVER and HEADLESS SERVER. They were recorded as unwritten from
# M0 to M11.d. This rung's delta to the specification adds that each "SHALL exist as a test in the
# kind whose location and budget fit it", so each one below states its kind and the reason — none is
# a kind of its own, because the taxonomy's rule for a marginal case is to move it to the kind whose
# budget fits and to say so.
#
#   scenario           kind         budget          why that kind
#   strategy stress    smoke        30 s per case   101 000 entities, 5 000 groups and 23 ticks of
#                                                   6 400 validated commands: well over integration's
#                                                   one second, well under smoke's thirty
#   control handover   integration  1 s per case    four participants and one vehicle — a seam
#                                                   between gameplay, control and replay, and cheap
#   headless server    smoke        30 s per case   a real program, run for a fixed number of ticks
#                                                   at a fixed rate over 100 000 entities
#
# `testing-and-quality` also asks for benchmarks to "run nightly on stable hardware". The strategy
# stress defends a RATIO against a committed ceiling rather than a nanosecond figure, which is what
# lets it run in CTest on a busy machine at all; tests/acceptance/README.md says what that does and
# does not replace.

cy_add_test(
    NAME         acceptance_strategy_stress
    KIND         smoke
    SOURCES      test_strategy_stress.cpp
    DEPENDENCIES cy::ecs cy::gameplay)

cy_add_test(
    NAME         acceptance_control_handover
    KIND         integration
    SOURCES      test_control_handover.cpp
    DEPENDENCIES cy::gameplay)

# --- The headless server ----------------------------------------------------------------------------
#
# A dedicated-server BUILD CONFIGURATION, which the tree did not have: "headless" was a run-time
# display-server choice on binaries that link the renderer anyway. `cy_headless_server` links the
# simulation — ECS, gameplay, AI, physics — and the diagnostics that observe it, and NOTHING that
# draws, plays or presents. Two checks hold that, and both fail the BUILD, which is what
# `testing-and-quality` asks ("a dependency on any of them SHALL fail the build"):
#
#   1. at configure time, the DECLARED link closure is walked and a forbidden target refuses the
#      configure, naming the path that reached it;
#   2. after every link, `headless_closure.py` reads the LINKED binary's symbols and dynamic
#      dependencies, because a clean declaration does not prove a clean binary.
#
# Physics is Jolt, so the server needs CY_PHYSICS; the AI needs CY_AI. Without them there is no
# server to build, and the configure says so rather than declaring a test that could not pass.
if(NOT TARGET cy::physics-jolt OR NOT TARGET cy::ai)
    message(STATUS "tests/acceptance: the headless server needs CY_PHYSICS and CY_AI; "
                   "smoke.acceptance_headless_server is not declared.")
    return()
endif()

cy_add_module(
    NAME    cy_headless_server
    LAYER   tools
    TYPE    EXECUTABLE
    SOURCES headless_server/main.cpp
    PRIVATE_DEPENDENCIES
        cy::ai
        cy::gameplay
        cy::graph
        cy::physics-jolt
        cy::servers-physics
        cy::core-diagnostics
        cy::core-memory)

find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_custom_command(TARGET cy_headless_server POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/headless_closure.py"
                "$<TARGET_FILE:cy_headless_server>"
        COMMENT "headless server: checking the linked closure for rendering, audio and interface"
        VERBATIM)
endif()

# The declared closure is walked by the SAME check `gameplay-framework`'s headless requirement
# already runs over every gameplay module — `cy_require_headless`, src/gameplay/cmake/headless.cmake
# — so one forbidden list decides what a dedicated server may not link, not two that drift apart.
# Deferred: the closure's targets are declared all over the tree, some after this directory.
function(cy_check_headless_server_closure)
    if(NOT COMMAND cy_require_headless)
        message(FATAL_ERROR "tests/acceptance: cy_require_headless is not defined; "
                            "src/gameplay/cmake/headless.cmake is where it lives")
    endif()
    cy_require_headless(cy_headless_server)
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_check_headless_server_closure)

if(Python3_Interpreter_FOUND)
    add_test(NAME smoke.acceptance_headless_server
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/headless_server.py"
                     --server "$<TARGET_FILE:cy_headless_server>"
                     --work "${CMAKE_CURRENT_BINARY_DIR}/headless-server"
             WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(smoke.acceptance_headless_server PROPERTIES
        LABELS "smoke" TIMEOUT "${CY_TEST_TIMEOUT_smoke}")
endif()
