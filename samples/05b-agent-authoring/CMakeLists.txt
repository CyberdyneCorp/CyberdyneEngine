# samples/05b-agent-authoring — the M5.5 artefact an agent drives. Task 4.2.
#
# --- WHY THIS DIRECTORY DECLARES NO cy_add_module() TARGET ----------------------------------------
#
# The same reason samples/05-editor-session declares none, and its CMakeLists.txt argues it at
# length: what is being demonstrated is a conversation between two processes — an agent and the
# editor, over the Model Context Protocol on standard input and output — and a C++ driver would link
# no engine code and prove nothing about the engine. `authoring.py` is the artefact and
# `just run-agent-authoring` is the recipe; this file declares the one CTest entry that runs it.
#
# The consequence, stated because somebody will look for it: `just run-sample agent-authoring` does
# NOT work. That recipe finds `samples/*/cy_sample_<name>` in the build tree and there is no such
# binary here.
#
# --- WHY IT IS `smoke` AND WHY IT NEEDS NEITHER A DISPLAY NOR A GPU --------------------------------
#
# It is the half of M5.5 that runs in continuous integration. The window artefact next door needs an
# X server, XTEST and a graphics device and says so; this one starts `cyberdyne-editor --mcp`, which
# opens no window and creates no device, and talks to it. That is why the milestone has two artefacts
# rather than one — `design.md` §5 — and why only this one is in the permanent gate set.

if(NOT CY_BUILD_TESTS)
    return()
endif()

# --- Where the artefact cannot run, and why that is an absence rather than a skip ------------------
#
# `cargo` builds the editor the session drives and `just` is how it is built. Where one of these is
# missing the entry is NOT DECLARED, the configure says so in one line, and the test list is visibly
# shorter — which is what bindings/swift/, samples/04-character/ and samples/05-editor-session/ all
# do. A registered test that skipped would report green on exactly the machines least able to judge
# the claim.
find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
find_program(CY_AGENT_CARGO cargo)
find_program(CY_AGENT_JUST just)
if(NOT Python3_Interpreter_FOUND OR NOT CY_AGENT_CARGO OR NOT CY_AGENT_JUST)
    message(STATUS
        "samples/05b-agent-authoring: the M5.5 agent artefact needs python3, cargo and just; it is "
        "not registered.\n"
        "   python3: ${Python3_Interpreter_FOUND}  cargo: ${CY_AGENT_CARGO}  "
        "just: ${CY_AGENT_JUST}")
    return()
endif()

# The workflow profile this configuration belongs to, inverted out of cmake/profiles.cmake rather
# than restated — the same lookup samples/05-editor-session does, and for the same reason: the driver
# hands the profile to `just build-editor`, and running an editor built in a different profile from
# the one under test is exactly the mistake the profile table exists to prevent.
set(CY_AGENT_PROFILE "")
foreach(candidate IN LISTS CY_PROFILES)
    if(CY_PROFILE_${candidate}_CONFIG STREQUAL CMAKE_BUILD_TYPE)
        set(CY_AGENT_PROFILE "${candidate}")
    endif()
endforeach()
if(NOT CY_AGENT_PROFILE)
    message(FATAL_ERROR
        "samples/05b-agent-authoring: no workflow profile maps to configuration "
        "'${CMAKE_BUILD_TYPE}'. cmake/profiles.cmake owns that table; if it was renamed, this "
        "lookup should follow it rather than be replaced by a literal.")
endif()

# The budget comes out of the taxonomy in tests/CMakeLists.txt rather than being restated here, which
# is what samples/04-character/ and samples/05-editor-session/ do: a budget copied into a second file
# drifts from the taxonomy that owns it. Only the timeout applies — cy::test's per-case CPU budget is
# compiled into a C++ test binary, and this entry is a process.
get_directory_property(CY_AGENT_TIMEOUT DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                       DEFINITION CY_TEST_TIMEOUT_smoke)
if(NOT CY_AGENT_TIMEOUT)
    message(FATAL_ERROR
        "samples/05b-agent-authoring: the test taxonomy's timeouts could not be read from "
        "tests/CMakeLists.txt. If those variables were renamed, the lookup should move with them.")
endif()

add_test(NAME smoke.agent_authoring
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/authoring.py"
                 --build --profile "${CY_AGENT_PROFILE}"
                 --work "${CMAKE_CURRENT_BINARY_DIR}/session"
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(smoke.agent_authoring PROPERTIES
                     LABELS "smoke" TIMEOUT "${CY_AGENT_TIMEOUT}")
