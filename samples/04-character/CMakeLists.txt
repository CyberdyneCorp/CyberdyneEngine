# samples/04-character — the M4 milestone artefact. Tasks 5.1 to 5.4.
#
# --- WHY THIS DIRECTORY MAY VANISH FROM THE BUILD, AND WHY THAT IS BETTER THAN A STUB ---------------
#
# The sample's gameplay is a Swift shared library. Without a Swift toolchain there is no library to
# load, and a host that reported "no game module" and exited zero would be a sample that passes on
# exactly the machines least able to judge it. So the target is NOT DECLARED when Swift is absent,
# the configure says why, and the absence is visible at configure time rather than discovered by a
# green run that did nothing.
#
# This is the same arrangement bindings/swift/ uses for its suites and tests/render/ uses for a
# machine with no GPU, and it is `testing-and-quality`'s position throughout the tree: a check that
# skips is a check whose green means nothing.
#
# --- WHY THE GAME IS BUILT AT BUILD TIME AND NOT AT RUN TIME -----------------------------------------
#
# `bindings/swift/tools/cy_swift_module.py` writes a small SwiftPM package whose TARGET is named for
# the hot-reload generation and builds it, because SwiftPM takes `-module-name` from the target name
# and the reload model needs a different one per generation. This sample builds generation 0 and
# never reloads — reload is exercised by `integration.swift_reload`, over a fixture built for it — but
# it uses the same driver, so the sample and the reload suite cannot drift about how a game module is
# produced. It is also the reason `module.toml` names `libCyGame_g0.so`.

find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "samples/04-character: no Python 3 interpreter; the sample is not built")
    return()
endif()

set(CY_CHARACTER_DRIVER "${CMAKE_SOURCE_DIR}/bindings/swift/tools/cy_swift_module.py")

execute_process(COMMAND "${Python3_EXECUTABLE}" "${CY_CHARACTER_DRIVER}" --probe
                RESULT_VARIABLE character_swift_result
                OUTPUT_VARIABLE character_swift_output
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT character_swift_result EQUAL 0)
    message(STATUS
        "samples/04-character: no Swift toolchain, so the M4 artefact is not built.\n"
        "   Its gameplay IS a Swift module; there is nothing useful to build without one.\n"
        "   `just env-doctor` diagnoses this, including the case where swiftly wrote its "
        "environment line to ~/.profile and a non-login shell never reads it.")
    return()
endif()
message(STATUS "samples/04-character: ${character_swift_output}")

# The Swift configuration follows the engine's, so `just test-all --profile release` exercises
# `swiftc -O` rather than only `-Onone`. bindings/swift/CMakeLists.txt makes the same choice and says
# why: M4's hot-reload spike ran its matrix at both and the verdicts only stay identical if both are
# still built.
if(CMAKE_BUILD_TYPE STREQUAL "Profile" OR CMAKE_BUILD_TYPE STREQUAL "Shipping")
    set(CY_CHARACTER_SWIFT_CONFIGURATION release)
else()
    set(CY_CHARACTER_SWIFT_CONFIGURATION debug)
endif()

set(CY_CHARACTER_MODULE_DIR "${CMAKE_CURRENT_BINARY_DIR}/module")
set(CY_CHARACTER_MODULE_WORK "${CMAKE_CURRENT_BINARY_DIR}/module-build")
set(CY_CHARACTER_MODULE_LIBRARY "${CY_CHARACTER_MODULE_DIR}/libCyGame_g0.so")
set(CY_CHARACTER_MODULE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/module.toml")

# A GLOB, AND ONE OF THE TWO PLACES THE RULE AGAINST THEM DOES NOT APPLY. `build-system-and-platforms`
# forbids globbed TARGET SOURCES, because a file that silently joins a compilation is a build nobody
# can read. This is not a target's source list: SwiftPM decides what it compiles, and this list is
# only the DEPENDS of the custom command that invokes it. A hand-written list would go stale the first
# time somebody added a Swift file and wondered why the game had not changed.
# bindings/swift/CMakeLists.txt and src/core/reflect/ do the same, for the same reason.
file(GLOB CY_CHARACTER_GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/game/*.swift")
file(GLOB_RECURSE CY_CHARACTER_KIT_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/bindings/swift/Sources/*.swift")

add_custom_command(
    OUTPUT "${CY_CHARACTER_MODULE_LIBRARY}"
    COMMAND "${Python3_EXECUTABLE}" "${CY_CHARACTER_DRIVER}"
            --work "${CY_CHARACTER_MODULE_WORK}" --out "${CY_CHARACTER_MODULE_DIR}"
            --configuration "${CY_CHARACTER_SWIFT_CONFIGURATION}"
            --generation "0=${CMAKE_CURRENT_SOURCE_DIR}/game"
    DEPENDS "${CY_CHARACTER_DRIVER}" "${CMAKE_SOURCE_DIR}/bindings/swift/Package.swift"
            ${CY_CHARACTER_GAME_SOURCES} ${CY_CHARACTER_KIT_SOURCES}
    COMMENT "Building the 04-character game module (Swift)"
    VERBATIM)

add_custom_target(cy_sample_character_game ALL DEPENDS "${CY_CHARACTER_MODULE_LIBRARY}")

# The host. `just run-sample` finds a sample's binary as samples/*/cy_sample_<name>, so the target
# name is what the recipe's `character` argument means; there is no manifest to keep in step.
#
# LAYER tools, like every sample: it consumes the whole engine and nothing consumes it. It is also the
# only place in the tree that names the input server, the physics server, the audio server, the camera
# server, the gameplay command stream and the ABI loader at once — which is what a game is, and why
# the layer rule puts a game above all six rather than inside any of them.
cy_add_module(
    NAME cy_sample_character
    LAYER tools
    TYPE EXECUTABLE
    SOURCES
        host/main.cpp
        host/game.cpp
        host/contract.cpp
    PRIVATE_DEFINITIONS
        "CY_CHARACTER_MODULE_LIBRARY=\"${CY_CHARACTER_MODULE_LIBRARY}\""
        "CY_CHARACTER_MODULE_MANIFEST=\"${CY_CHARACTER_MODULE_MANIFEST}\""
    PRIVATE_DEPENDENCIES
        cy::abi
        cy::gameplay
        cy::servers-input
        cy::servers-audio
        cy::servers-camera
        cy::servers-physics
        cy::physics-reference
        cy::runtime
        cy::platform-host
        cy::platform-headless
        cy::platform-desktop-sdl3)

add_dependencies(cy_sample_character cy_sample_character_game)

# Jolt is behind CY_PHYSICS, so `--jolt` is behind it too — and the reference backend is what the
# sample runs on either way. That is deliberate rather than a fallback: `physics` requires that
# swapping the backend change no gameplay, and a sample that could only be run against one of them
# would not be evidence of anything. The `#if` in host/game.cpp guards the include and the two calls.
if(CY_PHYSICS)
    target_link_libraries(cy_sample_character PRIVATE cy::physics-jolt)
endif()

# --- The smoke test, declared HERE and not in tests/smoke/CMakeLists.txt ----------------------------
#
# Task 5.4. The source lives in tests/smoke/ with the other four, because that is where a reader looks
# for it and where the taxonomy's budget applies. The DECLARATION has to be here, and the reason is
# ordering: the root CMakeLists.txt adds tests/ before samples/, so `if(TARGET cy_sample_character)`
# in tests/smoke/ would be false at configure time — and this target does not exist at all on a
# machine with no Swift toolchain. bindings/swift/CMakeLists.txt declares `integration.swift_reload`
# the same way and for the same reason.
#
# The budget and timeout come out of tests/CMakeLists.txt rather than being restated, which is what
# bindings/swift/ does: a budget copied into a second file drifts from the taxonomy that owns it.
if(CY_BUILD_TESTS)
    get_directory_property(character_budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_smoke)
    get_directory_property(character_timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_smoke)
    if(NOT character_budget OR NOT character_timeout)
        message(FATAL_ERROR
            "samples/04-character: the 'smoke' entry of the test taxonomy could not be read from "
            "tests/CMakeLists.txt. If the budget variables were renamed, declare this suite from "
            "tests/smoke/ instead and move the ordering problem with it.")
    endif()
    set(CY_TEST_BUDGET_smoke "${character_budget}")
    set(CY_TEST_TIMEOUT_smoke "${character_timeout}")

    cy_add_test(
        NAME character_sample
        KIND smoke
        SOURCES "${CMAKE_SOURCE_DIR}/tests/smoke/test_character_sample.cpp"
        DEFINITIONS CY_SAMPLE_CHARACTER="$<TARGET_FILE:cy_sample_character>")
    add_dependencies(cy_test_smoke_character_sample cy_sample_character)
endif()

# --- Task 5.3: the sample contains no C++ gameplay code ---------------------------------------------
#
# Two halves, and neither is sufficient alone. This is the static one — a partition check over the
# directory and a search for the game's own tunables in the host's C++. The other half is the
# `--no-behaviours` run inside the smoke test above, which is what actually measures the claim.
# tools/check_no_cpp_gameplay.py argues the split at greater length.
add_test(NAME integration.character_no_cpp_gameplay
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_no_cpp_gameplay.py"
                 --sample "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(integration.character_no_cpp_gameplay
                     PROPERTIES LABELS "integration" TIMEOUT 300)

# AND THE CHECK'S OWN NEGATIVE CASES, AS A SECOND REGISTRATION. The check above can only ever report
# that the sample passes; it says nothing about whether the four rules still FIRE. That is the failure
# this tree has caught before — a parser that stopped recognising a `write_f32` call would pass a
# violating sample and a clean one alike, and comparing nothing to nothing succeeds. `--selftest`
# edits the live tree in memory and requires each rule to reject its own violation, with case 0 (the
# unedited sample passes) as the control that makes the other four mean anything.
#
# It is a SEPARATE test rather than a flag on the one above so that a failure names which half broke:
# "the sample gained C++ gameplay code" and "the check that would have noticed has stopped working"
# are different defects with different fixes. Both match `-R character_no_cpp_gameplay`, which is the
# command the `character-artefact` gate and m4.toml's `no-cpp-gameplay` criterion declare.
add_test(NAME integration.character_no_cpp_gameplay_selftest
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_no_cpp_gameplay.py"
                 --sample "${CMAKE_CURRENT_SOURCE_DIR}" --selftest)
set_tests_properties(integration.character_no_cpp_gameplay_selftest
                     PROPERTIES LABELS "integration" TIMEOUT 300)
