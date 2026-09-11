# samples/09-multiplayer — the M9 milestone artefact. Section 6.
#
# A NINTH SHAPE, and it is the shape of the milestone: one program that is five machines, and one
# driver that judges what happened between them.
#
#   cy_sample_multiplayer  the session. Four clients and a host on one seeded, lossy datagram
#                          substrate; the host's authoritative log; a client that predicts, learns
#                          it was wrong, and rolls back; the log replayed bit-exactly; a divergence
#                          injected on purpose and narrowed to a field; and the crash replay buffer
#                          flushed into an artefact that re-simulates to its own hash.
#   multiplayer.py         the driver: it checks every claim the program printed, paints the two
#                          pictures from the program's own trace, and reports through
#                          samples/harness/artefact.py — which is what makes a recorded gap a
#                          non-zero exit and refuses an extreme value as a headline.
#
# `just run-multiplayer` is the recipe. `just run-sample multiplayer` runs the program alone.
#
# ================================================================================================
# WHY THIS IS A SECOND SAMPLE AND NOT AN EXTENSION OF samples/08-vertical-slice
# ================================================================================================
#
# The proposal states it and this directory is where it becomes a build decision: the subject is
# what happens BETWEEN machines. `samples/08-vertical-slice` is the single-process artefact and
# stays that way; a slice that ran in one process could only ever assert that it would have
# reconciled, and this milestone's whole complaint is about claims that exceed what was checked.
#
# ================================================================================================
# AND WHY IT DECLARES NOTHING AT ALL WITH CY_NETWORKING OFF
# ================================================================================================
#
# `-D CY_NETWORKING=OFF` removes src/networking/ from the build entirely, so `cy::networking` does
# not exist and a sample that named it anyway would turn "this feature is removable" into a
# CONFIGURE ERROR naming the artefact — which is precisely the shape M8.b's spike found in
# `-D CY_RENDERER_VULKAN=OFF` and which samples/08-vertical-slice's own CMakeLists.txt argues at
# length. The option must remove the transport; it must not remove the tree's ability to configure.
#
# What that costs is stated rather than hidden: with the option off there is no multiplayer sample
# and no `integration.multiplayer_session`, and `just run-multiplayer` reports an absent binary
# rather than a defect. The ledger runs the artefact in the default configuration, where
# CY_NETWORKING is ON.

if(NOT CY_NETWORKING)
    message(STATUS "samples/09-multiplayer: CY_NETWORKING is off, so the four-player session — "
                   "which is a session between machines — is not declared.")
    return()
endif()

cy_add_module(
    NAME cy_sample_multiplayer
    LAYER tools
    TYPE EXECUTABLE
    SOURCES
        main.cpp
        acts.cpp
        net.cpp
        world.cpp
    PRIVATE_DEPENDENCIES
        cy::core-determinism
        cy::ecs
        cy::gameplay
        cy::networking
        cy::replay)

# THE SAME FLAG `cy_replay` AND `cy_networking` CARRY, AND FOR THE SAME REASON. This program hashes
# floating-point state and compares the hash against a hash produced by a different translation
# unit in a different run; §1.2 of the milestone's design says the determinism profile is a
# BUILD-CONFIGURATION contract rather than only a source contract, and a sample that computed its
# own state under contraction would be the first place that stopped being true.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cy_sample_multiplayer PRIVATE -ffp-contract=off)
elseif(MSVC)
    target_compile_options(cy_sample_multiplayer PRIVATE /fp:precise)
endif()

if(NOT CY_BUILD_TESTS)
    return()
endif()

find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "samples/09-multiplayer: python3 is absent, so integration.multiplayer_session "
                   "is not declared.")
    return()
endif()

get_directory_property(CY_MULTIPLAYER_TIMEOUT DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                       DEFINITION CY_TEST_TIMEOUT_integration)
if(NOT CY_MULTIPLAYER_TIMEOUT)
    message(FATAL_ERROR
        "samples/09-multiplayer: the test taxonomy's timeouts could not be read from "
        "tests/CMakeLists.txt. If those variables were renamed, this lookup should move with them.")
endif()

# `integration` AND NOT `smoke`, and the taxonomy's rule is about cost: this entry builds five
# worlds, five transports and a seeded substrate, and runs a hundred and twenty ticks of all of
# them plus three readings of the log. It is nowhere near a millisecond and it does not belong in
# the unit tier or in the smoke tier's budget.
#
# It runs the WHOLE artefact on a shorter session — `--ticks` is the one dial whose cost is linear
# in everything measured — so a smaller run checks every claim's SHAPE and `just run-multiplayer`
# checks it at the length a reader is shown. It needs no display and no graphics device: there is
# no renderer anywhere in this artefact's link closure, which is what makes it judgeable on a
# hosted runner.
add_test(NAME integration.multiplayer_session
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/multiplayer.py"
                 --sample "$<TARGET_FILE:cy_sample_multiplayer>"
                 --work "${CMAKE_CURRENT_BINARY_DIR}/run"
                 --ticks 120
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(integration.multiplayer_session PROPERTIES
                     LABELS "integration" TIMEOUT "${CY_MULTIPLAYER_TIMEOUT}")
