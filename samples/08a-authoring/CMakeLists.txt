# samples/08a-authoring — M8.a's closing artefact. Section 6.
#
# --- A SEVENTH SHAPE, AND WHY THE SPLIT IS THE MILESTONE'S ----------------------------------------
#
# M8.a's claim is a sentence a person said: *"add Box, Cylinder, Sphere in a scene, add physics,
# place the object at different positions and play the scene."* Two things have to be true for that
# and neither demonstrates the other.
#
#   authoring.py            THE EDITOR DOES IT. One editor process and one engine runtime, over the
#                           editor's own control socket, driven through the command registry: the
#                           box and the sphere are created, one is placed above the other, a body is
#                           added to each, play is pressed, play is stopped, and undo empties the
#                           world. Every act is a command a person's editor invokes, and the
#                           runtime's own report is the second witness.
#   cy_sample_authoring     WHAT FELL, MEASURED. The runtime counts sessions and ticks; it does not
#                           count metres. This program plays the SAME authored world — the empty
#                           `.cyworld` plus the editor's own committed transactions, replayed with
#                           `cy::scene::serialization::apply_transaction` — and reports where the
#                           sphere started, where it came to rest, whether that is the height the
#                           two colliders imply, and whether `stop()` put the file back byte for
#                           byte. It needs no display and no graphics device, which makes it the
#                           half a hosted runner can judge.
#
# `just run-authoring` is the recipe. `just run-sample authoring` runs the measuring half alone,
# which is what a person debugging a fall wants.
#
# LAYER tools, like every sample. This one links the two modules M8.a brought — `cy::gameplay-play`
# and, through it, `cy::physics-bridge` — plus the serialization module section 1 made the engine's
# own reader and writer of `.cyworld`.

cy_add_module(
    NAME cy_sample_authoring
    LAYER tools
    TYPE EXECUTABLE
    ALIAS cy::sample-authoring
    SOURCES play.cpp
    PRIVATE_DEPENDENCIES
        cy::gameplay-play
        cy::scene-serialization
        cy::physics-reference)

# Jolt, and the macro that tells play.cpp it is there. The two lines go together and the second is
# not redundant: `CY_PHYSICS` is a CMake option and not a compile definition, so a build that links
# Jolt without this would compile a program whose `#if defined(CY_PHYSICS)` is false and which then
# reports the reference backend on a machine that has the real one. samples/04-character and
# samples/05b-editor-window/runtime both make the same pair, and this cost one confused run at M8.a.
#
# The reference backend is linked unconditionally above, because `physics` requires that swapping
# the backend change no gameplay — so `--physics reference` has to exist everywhere for that claim
# to be checkable, and because it is the only backend a build with CY_PHYSICS=OFF has.
if(CY_PHYSICS)
    target_link_libraries(cy_sample_authoring PRIVATE cy::physics-jolt)
    target_compile_definitions(cy_sample_authoring PRIVATE CY_PHYSICS=1)
endif()

if(NOT CY_BUILD_TESTS)
    return()
endif()

# --- Where the artefact cannot run, and why that is an absence rather than a skip ------------------
#
# The driver starts `cyberdyne-editor` and `cy_editor_window_runtime`, so it needs python3, cargo and
# just — the same three samples/05b-agent-authoring names, for the same reason. Where one is missing
# the entry is NOT DECLARED, the configure says so in one line, and the test list is visibly shorter.
# A registered test that skipped would report green on exactly the machines least able to judge it.
find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
find_program(CY_AUTHORING_CARGO cargo)
find_program(CY_AUTHORING_JUST just)
if(NOT Python3_Interpreter_FOUND OR NOT CY_AUTHORING_CARGO OR NOT CY_AUTHORING_JUST)
    message(STATUS
        "samples/08a-authoring: the M8.a artefact needs python3, cargo and just; it is not "
        "registered.\n"
        "   python3: ${Python3_Interpreter_FOUND}  cargo: ${CY_AUTHORING_CARGO}  "
        "just: ${CY_AUTHORING_JUST}")
    return()
endif()

# The workflow profile this configuration belongs to, inverted out of cmake/profiles.cmake rather
# than restated — the same lookup samples/05-editor-session and samples/05b-agent-authoring do, and
# for the same reason: the driver hands the profile to `just build-editor`, and running an editor
# built in a different profile from the one under test is the mistake the profile table exists to
# prevent.
set(CY_AUTHORING_PROFILE "")
foreach(candidate IN LISTS CY_PROFILES)
    if(CY_PROFILE_${candidate}_CONFIG STREQUAL CMAKE_BUILD_TYPE)
        set(CY_AUTHORING_PROFILE "${candidate}")
    endif()
endforeach()
if(NOT CY_AUTHORING_PROFILE)
    message(FATAL_ERROR
        "samples/08a-authoring: no workflow profile maps to configuration '${CMAKE_BUILD_TYPE}'. "
        "cmake/profiles.cmake owns that table; if it was renamed, this lookup should follow it "
        "rather than be replaced by a literal.")
endif()

get_directory_property(CY_AUTHORING_TIMEOUT DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                       DEFINITION CY_TEST_TIMEOUT_smoke)
if(NOT CY_AUTHORING_TIMEOUT)
    message(FATAL_ERROR
        "samples/08a-authoring: the test taxonomy's timeouts could not be read from "
        "tests/CMakeLists.txt. If those variables were renamed, this lookup should move with them.")
endif()

# THE SMOKE ENTRY RUNS THE WHOLE ARTEFACT, both halves, and reports the acts it cannot reach on this
# machine rather than passing. `--binaries` hands the driver what this configuration just built, so
# nothing here builds anything and the test reports on the tree under test. `--no-window` leaves out
# the photograph, which is the one act that needs a display; `just run-authoring` takes it.
add_test(NAME smoke.authoring
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/authoring.py"
                 --profile "${CY_AUTHORING_PROFILE}"
                 --work "${CMAKE_CURRENT_BINARY_DIR}/run"
                 --binaries
                 --sample "$<TARGET_FILE:cy_sample_authoring>"
                 --build-dir "${CMAKE_BINARY_DIR}"
                 --no-window
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(smoke.authoring PROPERTIES
                     LABELS "smoke" TIMEOUT "${CY_AUTHORING_TIMEOUT}")
