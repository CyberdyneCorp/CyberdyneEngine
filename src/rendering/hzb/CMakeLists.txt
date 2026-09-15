# src/rendering/hzb/ — layer 4: the hierarchical depth buffer, on the device. M11.c task 4.1.
#
# `rendering-culling-and-lod` — "Occlusion culling": "The engine SHALL support occlusion culling
# using a **hierarchical depth buffer** built from the previous frame's depth."
#
# WHY THIS IS A MODULE OF ITS OWN AND NOT PART OF EITHER MODULE THAT WAS WAITING FOR IT.
#
# Two rows were waiting on exactly one piece of work, and M11.c's design said so before it was
# built: `rendering-culling-and-lod`'s occlusion cull, where `GpuCullPass::upload` refused
# `kGpuCullOcclusion` by name because no pyramid existed on a device; and `virtual-geometry`'s
# cluster-granular occlusion, whose seam was already cut with
# `TraversalStatistics::nodes_pruned_by_occlusion` reading zero. Putting the pyramid in gpu_culling/
# would make virtual geometry link a culling dispatch it has no use for; putting it in
# virtual_geometry/ would do the reverse; building it twice would let ONE piece of work be recorded
# as TWO satisfied requirements, which is the failure mode the specification delta names in as many
# words.
#
# So it is here, and "one buffer, two consumers" is a LINK-GRAPH FACT rather than a sentence:
# cy::rendering-gpu-culling and cy::rendering-virtual-geometry both depend on cy::rendering-hzb, and
# their shaders both `#include` shaders/hzb_sample.slang.
#
# WHY NOT cy::servers-render-culling, WHICH ALREADY HOLDS AN Hzb. That module is layer 2 and may not
# name a device. What it holds is the pyramid's SHAPE and its TEST — the CPU model this module is
# checked against, tested since M6 — and this module is the pass. The split is hzb.h's and this file
# only obeys it.
#
# THE SPIR-V IS CHECKED IN, not compiled by the build, for the reason gpu_culling/ gives: shader
# compilation is off in Profile and Shipping, and an occlusion cull that only exists where Slang was
# built is a renderer with two different frames. shaders/hzb_reduce.slang carries the exact slangc
# invocation and shaders/embed_spirv.py writes src/hzb_spirv.h.
cy_add_module(
    NAME cy_rendering_hzb
    LAYER rendering
    ALIAS cy::rendering-hzb
    TYPE STATIC
    SOURCES src/hzb_pass.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-graph
                        cy::servers-render-culling
    PRIVATE_DEPENDENCIES cy::rhi)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
