# samples/05-editor-session — the M5 milestone artefact. Tasks 6.1, 6.2 and 6.3.
#
# --- WHY THIS DIRECTORY DECLARES NO cy_add_module() TARGET -----------------------------------------
#
# Every earlier artefact is one C++ executable, because every earlier milestone's subject was the
# engine. M5's subject is the boundary between two processes in two languages, and the claim being
# demonstrated — kill the hosted runtime mid-session and the editor survives with the document
# intact — is a statement about processes. A C++ driver would link no engine code and prove nothing
# about the engine; what it would add is a build dependency that says nothing. So the artefact is
# session.py, which orchestrates three real binaries, and this file only declares the two CTest
# entries that run it. README.md argues the shape at greater length.
#
# The consequence, stated because somebody will look for it: `just run-sample editor-session` does
# NOT work. That recipe finds `samples/*/cy_sample_<name>` in the build tree and there is no such
# binary here. `just run-editor-session` is the recipe.
#
# --- WHY THE TESTS ARE DECLARED HERE AND NOT FROM tests/editor/ ------------------------------------
#
# tests/editor/README.md holds the argument and the reason is ownership: the change that landed the
# artefact does not own tests/CMakeLists.txt, and a `CMakeLists.txt` under tests/editor/ that
# nothing adds is a file that only looks declared. samples/04-character declares
# `smoke.character_sample` from its own directory too, for a different reason — ordering — and says
# so where it does it. Whoever next edits tests/CMakeLists.txt should add `add_subdirectory(editor)`
# and move these two declarations into it; nothing else changes.

if(NOT CY_BUILD_TESTS)
    return()
endif()

# --- Where the artefact cannot run, and why that is an absence rather than a skip ------------------
#
# The live bridge is a Unix domain socket; `cy-runtime-stub` refuses to pretend otherwise on a
# platform that has none, and session.py exits 2 there rather than reporting a session it did not
# run. `cargo` builds the editor the session drives and `just` is how it is built. Where one of
# these is missing the entries are NOT DECLARED, the configure says so in one line, and the test
# list is visibly shorter — which is what bindings/swift/, samples/04-character/ and tests/render/
# all do. A registered test that skipped would report green on exactly the machines least able to
# judge the claim.
if(NOT UNIX)
    message(STATUS "samples/05-editor-session: not a Unix host, so the live bridge's socket does "
                   "not exist; the session is not registered")
    return()
endif()
if(NOT TARGET cy_import_cli)
    message(STATUS "samples/05-editor-session: cy_import_cli is not in this build "
                   "(CY_BUILD_TOOLS is off); the session is not registered")
    return()
endif()

find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
find_program(CY_SESSION_CARGO cargo)
find_program(CY_SESSION_JUST just)
if(NOT Python3_Interpreter_FOUND OR NOT CY_SESSION_CARGO OR NOT CY_SESSION_JUST)
    message(STATUS
        "samples/05-editor-session: the M5 artefact needs python3, cargo and just; it is not "
        "registered.\n"
        "   python3: ${Python3_Interpreter_FOUND}  cargo: ${CY_SESSION_CARGO}  "
        "just: ${CY_SESSION_JUST}")
    return()
endif()

# The workflow profile this configuration belongs to, inverted out of cmake/profiles.cmake rather
# than restated: the session hands it to `just build-editor`, and a second copy of that mapping is a
# second thing to keep in step. Getting it wrong would run an editor built in a different profile
# from the one under test, which is exactly the mistake the profile table exists to prevent.
set(CY_SESSION_PROFILE "")
foreach(candidate IN LISTS CY_PROFILES)
    if(CY_PROFILE_${candidate}_CONFIG STREQUAL CMAKE_BUILD_TYPE)
        set(CY_SESSION_PROFILE "${candidate}")
    endif()
endforeach()
if(NOT CY_SESSION_PROFILE)
    message(FATAL_ERROR
        "samples/05-editor-session: no workflow profile maps to configuration "
        "'${CMAKE_BUILD_TYPE}'. cmake/profiles.cmake owns that table; if it was renamed, this "
        "lookup should follow it rather than be replaced by a literal.")
endif()

set(CY_SESSION_DRIVER "${CMAKE_CURRENT_SOURCE_DIR}/session.py")

# smoke.editor_session — the whole artefact. It builds the editor itself (a Cargo build of a
# workspace with no third-party dependencies: 3.4 s cold, about 0.2 s warm) and is handed the
# importer as a built target, because running `just build-tools` from inside a CTest run would
# reconfigure the tree ctest is reading its own test list out of.
#
# The budget and the timeout come out of the taxonomy in tests/CMakeLists.txt rather than being
# restated here, which is what bindings/swift/ and samples/04-character/ do: a budget copied into a
# second file drifts from the taxonomy that owns it. Only the timeout applies — cy::test's per-case
# CPU budget is compiled into a C++ test binary, and this entry is a process.
get_directory_property(CY_SESSION_TIMEOUT DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                       DEFINITION CY_TEST_TIMEOUT_smoke)
get_directory_property(CY_SESSION_TIMEOUT_INTEGRATION DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                       DEFINITION CY_TEST_TIMEOUT_integration)
if(NOT CY_SESSION_TIMEOUT OR NOT CY_SESSION_TIMEOUT_INTEGRATION)
    message(FATAL_ERROR
        "samples/05-editor-session: the test taxonomy's timeouts could not be read from "
        "tests/CMakeLists.txt. If those variables were renamed, declare these entries from "
        "tests/editor/ instead and move the lookup with them.")
endif()

add_test(NAME smoke.editor_session
         COMMAND "${Python3_EXECUTABLE}" "${CY_SESSION_DRIVER}"
                 --build --profile "${CY_SESSION_PROFILE}"
                 --import-cli "$<TARGET_FILE:cy_import_cli>"
                 --work "${CMAKE_CURRENT_BINARY_DIR}/session"
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(smoke.editor_session PROPERTIES
                     LABELS "smoke" TIMEOUT "${CY_SESSION_TIMEOUT}")

# AND THE DRIVER'S OWN NEGATIVE CASES, AS A SEPARATE ENTRY. The test above can only ever report that
# the session passed; it says nothing about whether its two load-bearing assertions still FIRE. This
# one requires act 4 to reject a runtime that was never killed and act 3 to reject a session whose
# runtime never answered. It is separate rather than a flag on the one above so that a failure names
# which half broke: "the editor stopped surviving its runtime" and "the check that would have
# noticed has stopped working" are different defects with different fixes.
add_test(NAME integration.editor_session_selftest
         COMMAND "${Python3_EXECUTABLE}" "${CY_SESSION_DRIVER}" --selftest
                 --build --profile "${CY_SESSION_PROFILE}"
                 --import-cli "$<TARGET_FILE:cy_import_cli>"
                 --work "${CMAKE_CURRENT_BINARY_DIR}/session-selftest"
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(integration.editor_session_selftest PROPERTIES
                     LABELS "integration" TIMEOUT "${CY_SESSION_TIMEOUT_INTEGRATION}")
