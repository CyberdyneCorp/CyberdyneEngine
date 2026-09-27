# samples/13-rts-selection — a strategy game's selection, drawn by the engine.
#
# A squad selected by a drag box, an enemy under the cursor, and a unit behind a building, marked
# from gameplay as `SelectionHighlight` components on their entities and outlined by
# `selection::OutlinePass` in the engine's own assembled frame. main.cpp says what it runs.
#
# THE SCENE IS THE PIPELINE SUITES' OWN. `src/rendering/pipeline/tests/frame_scene.cpp` is compiled
# here as `tests/render/golden.cpp` is compiled by samples/12-beauty: the frame the suites hold to
# their references is the frame this picture is taken of, rather than a second scene that drifts.
#
# Vulkan only, and declared only with it: the program is a picture, and a sample whose subject is
# absent declares nothing rather than a target that cannot draw (samples/12-beauty's rule). No
# CTest entry, for the reason every capture in this tree gives; `render.selection_outlines` is the
# suite.
if(NOT CY_RENDERER_VULKAN)
    message(STATUS "samples/13-rts-selection: CY_RENDERER_VULKAN is off, so the selection picture "
                   "is not declared.")
    return()
endif()

cy_add_module(
    NAME cy_sample_rts_selection
    LAYER tools
    TYPE EXECUTABLE
    SOURCES
        main.cpp
        "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests/frame_scene.cpp"
        "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
    PRIVATE_DEPENDENCIES
        cy::ecs
        cy::rendering-particles
        cy::rendering-pipeline
        cy::rendering-selection
        cy::rhi
        cy::rhi-null
        cy::rhi-vulkan)

target_include_directories(cy_sample_rts_selection
                           PRIVATE "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests"
                                   "${CMAKE_SOURCE_DIR}/tests/render")
