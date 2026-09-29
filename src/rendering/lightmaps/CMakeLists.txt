# src/rendering/lightmaps/ — layer 4: a baked lightmap in the forward frame.
#
# `rendering-global-illumination` — "Lightmap baking". The bake is cy::rendering-lightmap-bake,
# which runs every case headless and links no device; the frame is cy::rendering-pipeline, which
# knows nothing of GI. This module is where the two meet, for the reason src/rendering/light_probes/
# gives: the atlas planes as textures, the view-block words that describe them, and the one rule —
# read off gi::exclusion_for() — for which ambient source a surface takes.
#
# NO SHADER OF ITS OWN. The sampling is cy/frame.slang's `lightmapAmbient`, in the forward
# fragment's committed SPIR-V and MSL; this module only fills what that function reads.
cy_add_module(
    NAME cy_rendering_lightmaps
    LAYER rendering
    ALIAS cy::rendering-lightmaps
    TYPE STATIC
    SOURCES src/lightmap_textures.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    # cy::servers-render for `render::DebugViewMode`, which the public header's
    # `write_lightmap_debug_view` takes: named here rather than reached through the pipeline.
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-gi
                        cy::rendering-lightmap-bake cy::rendering-pipeline cy::rhi
                        cy::servers-render
    PRIVATE_DEPENDENCIES cy::rendering-graph)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
