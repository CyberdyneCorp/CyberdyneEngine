# src/rendering/post/ — layer 4: the post-process chain, in its defined order. Task 8.4.
#
# `rendering-post-processing` at Working. The order is the requirement — fifteen steps, linear HDR
# scene colour until tonemapping — and it is DATA here: `PostStage`'s enumerator values are the
# order, and `build_post_chain()` walks the enumeration once. A chain cannot come out in a different
# order than the specification lists, and moving a stage is a diff a reviewer sees.
#
# NO DEVICE AND NO SHADER. What is here is the arithmetic that decides what each effect DOES: the
# circle of confusion from a real lens, the froxel depth distribution and its inverse, the histogram
# percentile that stops a window blowing out a room, the tone curves, the grade and its bake, the
# soft bloom threshold, the volume blend, and the declared cost each stage contributes to a frame
# budget. Those are the parts that are wrong when an effect looks wrong, and every one of them is
# checkable without a GPU. The gathers, blurs and downsample chains are shaders and belong with the
# frame's passes.
#
# cy::rendering-temporal is a dependency because two stages of this chain are its consumers, and
# src/temporal_binding.cpp is where that consumption is written down: the chain's temporal stage gets
# its history by ASKING, through a ConsumerId only the framework can mint. `rendering-post-processing`
# says TAA "SHALL consume the temporal framework… It SHALL NOT implement its own", and nothing in
# this module allocates a history buffer or owns a jitter sequence — which is the property that
# sentence is about.
cy_add_module(
    NAME cy_rendering_post
    LAYER rendering
    ALIAS cy::rendering-post
    TYPE STATIC
    SOURCES
        src/chain.cpp
        src/effects.cpp
        src/exposure.cpp
        src/grading.cpp
        src/quality.cpp
        src/temporal_binding.cpp
        src/tonemap.cpp
        src/volumes.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-temporal)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
