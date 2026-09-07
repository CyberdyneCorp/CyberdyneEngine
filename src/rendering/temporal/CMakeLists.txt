# src/rendering/temporal/ — layer 4: CyberTemporal, the ONE temporal framework. Task 8.3.
#
# `temporal-rendering` at Working. The word "one" is the requirement and it is the reason this is a
# module rather than a header inside a TAA pass: temporal antialiasing, temporal upscaling,
# screen-space reflections, screen-space GI, ambient occlusion, volumetric integration and shadow
# caching all need jitter, motion vectors, history, reprojection, disocclusion classification and
# invalidation, and specified per effect they get implemented per effect, disagree about
# conventions, and each carry their own history-invalidation bug.
#
# M7 design.md §5 lists it first among the things that must not be retrofitted: "Five reprojections
# that disagree about history invalidation is the defect that cannot be found from a screenshot."
#
# NO DEVICE. The framework decides what the jitter IS, what a motion vector MEANS, when history
# becomes invalid and what each pixel's history state is — all of which are arithmetic, and all of
# which are the parts that go wrong. The textures themselves are the render graph's to allocate;
# `HistoryResource::bytes()` is the accounting figure the diagnostics requirement asks for.
cy_add_module(
    NAME cy_rendering_temporal
    LAYER rendering
    ALIAS cy::rendering-temporal
    TYPE STATIC
    SOURCES
        src/framework.cpp
        src/history.cpp
        src/jitter.cpp
        src/motion.cpp
        src/reprojection.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
