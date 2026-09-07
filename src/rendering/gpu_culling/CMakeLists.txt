# src/rendering/gpu_culling/ — layer 4: GPU-driven culling's dispatch. M7 task 5.1.
#
# `rendering-culling-and-lod` — "GPU-driven culling": "instance bounds are uploaded once, culling
# runs as a compute pass producing compacted draw arguments, and drawing uses indirect commands."
#
# WHY THIS IS A MODULE AND NOT PART OF cy::rendering-culling. src/rendering/culling/ is the CPU
# visible-set builder and its README states, as a property of its dependency list rather than as a
# rule, that it names no device, no graph and no shader — which is what lets every case in its suite
# run headless on a machine with no GPU. This module names all three. Folding it in would put a
# compute pipeline in the module the forward renderer reaches for a visible set, and would make that
# module's headless property a thing somebody has to remember rather than a thing the link line says.
#
# WHY IT IS NOT PART OF cy::servers-render-culling EITHER. That module is layer 2 and may not name a
# device. It holds the LAYOUTS a dispatch reads and writes, plus `cpu_reference_cull` — a CPU
# implementation of exactly the algorithm the shader here runs. The split is that file's and this one
# only obeys it.
#
# THE SPIR-V IS CHECKED IN, not compiled by the build. shaders/gpu_cull.slang carries the two slangc
# invocations and shaders/embed_spirv.py turns their output into src/gpu_cull_spirv.h. That is the
# same arrangement tests/render/shaders/ and samples/03-first-light/shaders/ use, for the same
# reason: CY_SHADER_SLANG is off in Profile and Shipping, and a culling pass that only exists where
# Slang was built is a renderer with two different frames.
cy_add_module(
    NAME cy_rendering_gpu_culling
    LAYER rendering
    ALIAS cy::rendering-gpu-culling
    TYPE STATIC
    SOURCES src/cull_pass.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-graph
                        cy::servers-render cy::servers-render-culling
    PRIVATE_DEPENDENCIES cy::rhi)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
