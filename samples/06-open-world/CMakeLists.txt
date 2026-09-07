# samples/06-open-world — the M6 milestone artefact. Section 9.
#
# A FIFTH SHAPE, AND THE SPLIT IS THE MILESTONE'S OWN. M6 has two halves — a world larger than
# memory, and a build that is a graph — and neither can demonstrate the other on its own. So the
# directory declares ONE C++ target and ONE driver:
#
#   cy_sample_open-world   the game. It reads its world out of an INSTALLATION by logical name,
#                          streams a six-kilometre city along a fixed route, pages two virtual
#                          textures under the shared residency policy, records what the player
#                          changed in the persistence overlay, and writes it as a save.
#   openworld.py           the five acts around it: cook and package the content through
#                          `cy_build`, install it, play, quit, resume, then change one line of
#                          content, rebuild — watching which nodes hit the cache — ship the
#                          difference as a patch, apply it, and play the patched world.
#
# `just run-open-world` is the recipe. `just run-sample open-world` also works and runs the middle
# act alone, because this directory — unlike M5's and M5.5's — does declare a `cy_sample_<name>`
# binary; what it cannot do on its own is the packaging and the patch, which are two more processes.
#
# LAYER tools, like every sample. This one links `cy::build-graph` as well as the engine, which is
# what lets the game read its content through the manifest in force rather than off a path: layer 7
# may link any layer, and a sample consuming the tool that ships its content is the arrangement the
# milestone is about.

# The build tools are what ship this sample's content, and `cy::build-graph` is one of them. When
# they are switched off the artefact is NOT DECLARED and the configure says so in one line — the
# same answer samples/04-character gives for a machine with no Swift toolchain, and for the same
# reason: a target that silently lost its content pipeline would report on nothing.
if(NOT TARGET cy::build-graph)
    message(STATUS
        "samples/06-open-world: the M6 artefact reads its world out of an installation and needs "
        "cy::build-graph, which CY_BUILD_TOOLS=OFF excludes. It is not built.")
    return()
endif()

cy_add_module(
    NAME cy_sample_open-world
    LAYER tools
    TYPE EXECUTABLE
    ALIAS cy::sample-open-world
    SOURCES
        main.cpp
        content.cpp
        run.cpp
    PRIVATE_DEPENDENCIES
        cy::world
        cy::save
        cy::servers-residency
        cy::servers-render-virtual-texturing
        cy::build-graph)

if(NOT CY_BUILD_TESTS)
    return()
endif()

# --- The artefact as a test, and what it needs ----------------------------------------------------
#
# The driver runs `cy_build` and this binary as separate processes and needs python3 to do it. Where
# python3 is absent the entry is not declared and the test list is visibly shorter, which is what
# samples/05-editor-session and samples/05b-agent-authoring both do rather than registering a test
# that would skip on exactly the machines least able to judge the claim.
find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "samples/06-open-world: python3 is absent, so smoke.open_world is not declared.")
    return()
endif()

# The budget comes out of the taxonomy in tests/CMakeLists.txt rather than being restated here — a
# budget copied into a second file drifts from the one that owns it. Only the timeout applies:
# cy::test's per-case CPU budget is compiled into a C++ test binary and this entry is a process.
get_directory_property(CY_OPEN_WORLD_TIMEOUT DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                       DEFINITION CY_TEST_TIMEOUT_smoke)
if(NOT CY_OPEN_WORLD_TIMEOUT)
    message(FATAL_ERROR
        "samples/06-open-world: the test taxonomy's timeouts could not be read from "
        "tests/CMakeLists.txt. If those variables were renamed, this lookup should move with them.")
endif()

# THE SMOKE ENTRY RUNS THE WHOLE ARTEFACT, ALL FIVE ACTS, on a shortened route. `--binaries` hands
# the driver the two programs this configuration just built, so nothing here builds anything and the
# test reports on the tree under test rather than on whatever `just` would produce for it.
add_test(NAME smoke.open_world
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/openworld.py"
                 --sample "$<TARGET_FILE:cy_sample_open-world>"
                 --cy-build "$<TARGET_FILE:cy_build>"
                 --work "${CMAKE_CURRENT_BINARY_DIR}/run"
                 --ticks 1800
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(smoke.open_world PROPERTIES
                     LABELS "smoke" TIMEOUT "${CY_OPEN_WORLD_TIMEOUT}")
