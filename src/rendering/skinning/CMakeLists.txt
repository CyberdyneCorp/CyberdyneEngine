# src/rendering/skinning/ — layer 4: the skinning compute pass. M8.d.
#
# `rendering-geometry-and-resources` — "Skinning": "Skinned meshes SHALL be transformed by a compute
# pass writing into per-instance output vertex buffers, so the result is reusable across passes
# (depth, shadows, main) without re-skinning."
#
# WHY THIS IS A MODULE AND NOT PART OF cy::servers-render-geometry. That module is layer 2 and may
# not name a device, a graph or a shader — which is the property that lets every case in its suite
# run headless on a machine with no GPU. It holds the LAYOUTS a dispatch reads and writes, plus
# `cpu_reference_skin`, a CPU implementation of exactly the algorithm the shader here runs. This
# module names all three. The split is skin_dispatch.h's and this one only obeys it; it is the same
# split src/rendering/gpu_culling/ makes against src/servers/render/culling/, for the same reason.
#
# WHY IT IS NOT PART OF cy::rendering-pipeline EITHER. That module owns the pipeline state objects a
# frame's graphics passes bind and explicitly owns no geometry. A compute pass that writes vertex
# buffers is neither a pipeline state object nor a frame's recording; folding it in would put a
# compute dispatch inside the module whose whole claim is that it records the frame's draws.
#
# THE SPIR-V IS CHECKED IN, not compiled by the build. shaders/skin.slang carries the slangc
# invocation and shaders/embed_spirv.py turns its output into src/skin_spirv.h. That is the same
# arrangement src/rendering/gpu_culling/ and tests/render/shaders/ use, for the same reason:
# CY_SHADER_SLANG is off in Profile and Shipping, and skinning that only exists where Slang was
# built is a renderer with two different frames.
cy_add_module(
    NAME cy_rendering_skinning
    LAYER rendering
    ALIAS cy::rendering-skinning
    TYPE STATIC
    SOURCES src/skin_pass.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-graph
                        cy::servers-render cy::servers-render-geometry
    PRIVATE_DEPENDENCIES cy::rhi)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
