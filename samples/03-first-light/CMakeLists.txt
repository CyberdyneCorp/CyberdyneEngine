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
cy_add_module(
    NAME cy_sample_first-light
    LAYER tools
    TYPE EXECUTABLE
    SOURCES main.cpp
    PRIVATE_DEPENDENCIES
        cy::sample-first-light
        cy::rhi-null
        cy::runtime
        cy::platform-host
        cy::platform-headless
        cy::platform-desktop-sdl3)

if(CY_RENDERER_VULKAN)
    target_link_libraries(cy_sample_first-light PRIVATE cy::rhi-vulkan)
endif()
