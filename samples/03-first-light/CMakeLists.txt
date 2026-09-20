# samples/03-first-light — the M3 milestone artefact. Tasks 6.1 and 6.2.
#
# TWO TARGETS, AND THE SPLIT IS THE POINT. `cy::sample-first-light` is the scene and the renderer:
# no `main`, no command line, no platform. `cy_sample_first-light` is the host around it.
#
# The library exists so that tests/render/ can render THIS PROGRAM'S FRAME rather than a copy of it.
# Task 6.3's golden images photograph the library on a device and task 6.4 compiles the identical
# frame through the null backend, so a change that breaks the sample turns a gate red instead of
# being noticed the next time somebody runs it by hand. A second scene written for the tests would
# drift from this one within a milestone, and the golden images would then be photographs of
# something nobody ships.
#
# LAYER tools, like every sample: it consumes the whole engine and nothing consumes it — except the
# render suites, which are also LAYER tools, which is what makes the arrangement above legal rather
# than a hole in the layer rule.
#
# THE LIBRARY NAMES NO BACKEND. It depends on cy::rhi (the interface) and cy::rendering-graph, and
# on neither cy::rhi-null nor cy::rhi-vulkan: a renderer that linked a backend could not be run
# against the other one, and design.md §1's whole argument is that it must be. The executable below
# links the backends, because choosing one is the host's job.

cy_add_module(
    NAME cy_sample_first-light_scene
    LAYER tools
    ALIAS cy::sample-first-light
    SOURCES
        scene.cpp
        renderer.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    PUBLIC_DEPENDENCIES
        cy::core-math
        cy::rendering-graph)

# The host. `just run-sample` finds a sample's binary as samples/*/cy_sample_<name>, so the target
# name is what the recipe's `first-light` argument means; there is no manifest to keep in step.
#
# cy::rhi-null is linked unconditionally and cy::rhi-vulkan only when the feature is on, which is
# also what CY_RENDERER_VULKAN means to the sample's source: the one `#if` in main.cpp guards the
# explicit registration call and nothing else.
#
# EVERY PLATFORM BACKEND THIS BUILD HAS, because M11.d task 4.3 asks whether the M3 golden image is
# the same image when something other than SDL3 owns the process. `--platform` chooses at run time
# and `golden_legs.py` runs two of them; `samples/00-empty/CMakeLists.txt` says the same thing about
# the same four. The native backend is a conditional dependency and a compile definition rather than
# an unconditional link, because it is built only where it can be.
set(cy_sample_first_light_definitions CY_SAMPLE_LINUX_NATIVE=0)
set(cy_sample_first_light_platforms
    cy::platform-headless cy::platform-desktop-sdl3 cy::platform-stub)
if(TARGET cy_platform_linux_native)
    list(APPEND cy_sample_first_light_platforms cy::platform-linux-native)
    set(cy_sample_first_light_definitions CY_SAMPLE_LINUX_NATIVE=1)
endif()

cy_add_module(
    NAME cy_sample_first-light
    LAYER tools
    TYPE EXECUTABLE
    SOURCES main.cpp
    PRIVATE_DEFINITIONS ${cy_sample_first_light_definitions}
    PRIVATE_DEPENDENCIES
        cy::sample-first-light
        cy::rhi-null
        cy::runtime
        cy::platform-host
        ${cy_sample_first_light_platforms})

if(CY_RENDERER_VULKAN)
    target_link_libraries(cy_sample_first-light PRIVATE cy::rhi-vulkan)
endif()
if(CY_RENDERER_METAL)
    target_link_libraries(cy_sample_first-light PRIVATE cy::rhi-metal)
endif()
if(CY_RENDERER_D3D12)
    target_link_libraries(cy_sample_first-light PRIVATE cy::rhi-d3d12)
endif()

if(NOT CY_BUILD_TESTS)
    return()
endif()

# --- THE M3 GOLDEN IMAGE, ONCE PER PLATFORM BACKEND. M11.d task 4.3 ------------------------------
#
# `golden_legs.py` runs this binary on every platform backend the build has and checks each capture
# against `tests/render/references/first_light.png` — the reference `render.golden` judges — and
# against the other legs. Its own header says at length why the claim lives HERE and not in
# `tests/render/`: that directory links no platform target and constructs no `DisplayServer`, so no
# platform backend can change one texel of it, and putting a window into it would make the render
# suites need a display on every machine that runs them.
#
# `python3` absent means the entry is not declared, which is what samples/06-open-world,
# 05-editor-session, 05b-agent-authoring and 11-ship all do rather than registering a test that
# would skip on exactly the machines least able to judge the claim.
find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "samples/03-first-light: python3 is absent, so smoke.first_light_legs is not "
                   "declared.")
    return()
endif()

get_directory_property(CY_FIRST_LIGHT_TIMEOUT DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                       DEFINITION CY_TEST_TIMEOUT_smoke)
if(NOT CY_FIRST_LIGHT_TIMEOUT)
    message(FATAL_ERROR
        "samples/03-first-light: the test taxonomy's timeouts could not be read from "
        "tests/CMakeLists.txt. If those variables were renamed, this lookup should move with them.")
endif()

# SKIP_RETURN_CODE 3, as samples/05b-editor-window does: a machine with no graphics device cannot
# compare a picture and a machine with no display cannot run the windowed legs, and both of those
# are the MACHINE's limits rather than the subject's. A leg that could not run is reported NOT
# EVALUATED inside the run; a run where no leg at all could be judged exits 3 and CTest says SKIP.
add_test(NAME smoke.first_light_legs
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/golden_legs.py"
                 --sample "$<TARGET_FILE:cy_sample_first-light>"
                 --work "${CMAKE_CURRENT_BINARY_DIR}/legs"
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(smoke.first_light_legs PROPERTIES
                     LABELS "smoke" TIMEOUT "${CY_FIRST_LIGHT_TIMEOUT}" SKIP_RETURN_CODE 3)
