# samples/07-fidelity — the M7 milestone artefact. Section 11.
#
# A SIXTH SHAPE, and the split is the milestone's: M7's claim is that six unbounded systems become
# one frame, and neither the frame nor the negotiation can demonstrate the other.
#
#   cy_sample_fidelity   the set and the frame. It generates a film-detail interior and exterior,
#                        cooks every shell through `virtual-geometry`'s real builder with the
#                        crack-free check the cook is required to run, puts the result on the GPU
#                        through the cluster traversal and the visibility buffer, lights it with
#                        the real illumination system, and then drives the real budget arbiter with
#                        the frame time it just measured under a scripted load spike.
#   fidelity.py          the driver: it runs the four acts, checks every claim the program printed,
#                        and reports through samples/harness/artefact.py — which is what makes a
#                        recorded gap a non-zero exit and refuses an extreme value as a headline.
#
# `just run-fidelity` is the recipe. `just run-sample fidelity` runs the program alone.
#
# LAYER tools, like every sample. This one links five rendering modules and the RHI, which is what
# a milestone about the renderer's composition has to do: layer 7 may link any layer, and the point
# of the artefact is that these modules meet somewhere other than in their own suites.

# THREE TARGETS NOW, AND THE NEW SPLIT IS THE SAME ONE samples/03-first-light MADE AT M3.
# `cy::sample-fidelity-frame` is the set, the frame and the shading with no `main` around them, so
# that `render.virtual_geometry_shaded` under tests/render/ photographs THIS PROGRAM'S FRAME rather
# than a copy of it. A golden suite with a scene of its own drifts from the sample inside a
# milestone and its reference is then a photograph of something nobody ships — which is exactly the
# state `docs/design/images/virtual-geometry-shaded.png` was in before M11.c: written by a recipe,
# compared by nothing.
cy_add_module(
    NAME cy_sample_fidelity_frame
    LAYER tools
    ALIAS cy::sample-fidelity-frame
    SOURCES
        scene.cpp
        frame.cpp
        shade.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    PUBLIC_DEPENDENCIES
        cy::rendering-virtual-geometry
        cy::rendering-gi
        cy::rendering-shadows
        cy::rendering-graph
        cy::rhi
    PRIVATE_DEPENDENCIES
        cy::rhi-null)

# cy::rhi-null is linked unconditionally and cy::rhi-vulkan only when the feature is on, which is
# the arrangement samples/03-first-light settled: a sample that linked a backend it cannot use would
# not build on a machine without it, and one that linked none could not be run anywhere. The frame
# act asks for "vulkan" first and reports itself NOT EVALUATED when the device that answers is the
# null one — which is the honest report on a hosted runner and the reason the smoke entry exists.
#
# The define is PUBLIC because `frame.cpp` is the only translation unit that reads it and consumers
# do not — but a consumer that linked the library and not the backend would not link, so the
# dependency is public too.
if(CY_RENDERER_VULKAN)
    target_link_libraries(cy_sample_fidelity_frame PUBLIC cy::rhi-vulkan)
    target_compile_definitions(cy_sample_fidelity_frame PRIVATE CY_SAMPLE_FIDELITY_VULKAN=1)
endif()

cy_add_module(
    NAME cy_sample_fidelity
    LAYER tools
    TYPE EXECUTABLE
    ALIAS cy::sample-fidelity
    SOURCES
        main.cpp
        spike.cpp
    PRIVATE_DEPENDENCIES
        cy::sample-fidelity-frame
        cy::rendering-arbiter
        cy::rhi-null)

if(NOT CY_BUILD_TESTS)
    return()
endif()

find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "samples/07-fidelity: python3 is absent, so smoke.fidelity is not declared.")
    return()
endif()

get_directory_property(CY_FIDELITY_TIMEOUT DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                       DEFINITION CY_TEST_TIMEOUT_smoke)
if(NOT CY_FIDELITY_TIMEOUT)
    message(FATAL_ERROR
        "samples/07-fidelity: the test taxonomy's timeouts could not be read from "
        "tests/CMakeLists.txt. If those variables were renamed, this lookup should move with them.")
endif()

# THE SMOKE ENTRY RUNS THE WHOLE ARTEFACT, all four acts, on a smaller set: `--detail` scales the
# cooked resolution, which is the one dial whose cost is superlinear. `--binaries` hands the driver
# the program this configuration just built, so nothing here builds anything and the test reports on
# the tree under test rather than on whatever `just` would produce for it.
#
# It needs no display. It DOES use a graphics device where there is one, and reports the frame act
# as not evaluated where there is not — which is `delivery-roadmap`'s rule and the reason this entry
# is declared on a hosted runner at all.
add_test(NAME smoke.fidelity
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fidelity.py"
                 --sample "$<TARGET_FILE:cy_sample_fidelity>"
                 --work "${CMAKE_CURRENT_BINARY_DIR}/run"
                 --detail 0.5
                 --frames 24
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(smoke.fidelity PROPERTIES
                     LABELS "smoke" TIMEOUT "${CY_FIDELITY_TIMEOUT}")

# THE CAPTURE TOOL, which regenerates docs/design/images/virtual-geometry-*.png. It is declared
# beside the artefact because it renders the artefact's own scene through the artefact's own frame;
# a second copy of either would be a second thing to keep true. It compiles tests/render/golden.cpp
# directly rather than linking the test target, which is what src/vfx/tests does and what keeps one
# PNG encoder in the tree.
cy_add_module(
    NAME cy_fidelity_capture
    LAYER tools
    TYPE EXECUTABLE
    ALIAS cy::fidelity-capture
    SOURCES
        capture.cpp
        "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
    PRIVATE_DEPENDENCIES
        cy::sample-fidelity-frame
        cy::rhi-null)
target_include_directories(cy_fidelity_capture PRIVATE "${CMAKE_SOURCE_DIR}/tests/render")
