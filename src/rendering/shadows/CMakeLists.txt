# src/rendering/shadows/ — layer 4: CyberShadow. Tasks 8.1 and 8.2.
#
# `virtual-shadows` at Working: the virtual address space, receiver-driven page marking, directional
# clipmaps and their snapping, the physical page cache, precise invalidation, update classes and
# staleness, the shadow budget, derived bias and the fallback chain.
#
# NO DEVICE, NO GRAPH, NO SHADER — the same division src/rendering/culling/ and
# src/rendering/lighting/ draw, and for the same reason. Everything in this module is a decision
# about WHICH pages exist, WHEN they are re-rendered and WHAT they cost, and every one of those is
# arithmetic over positions, bounds and frame counts. A shadow that swims, a cache that never hits,
# a controller that oscillates and a fallback that stalls are all findable without a GPU, and
# `tests/` finds them headless. The dispatches that consume this — page marking over the depth
# buffer, GPU-driven caster selection, page-batched rasterisation — are the device half and belong
# with the frame's passes; the specification's requirement that they be GPU-driven is a statement
# about where the loop runs, and this module's interfaces are shaped so that the loop is a dispatch
# rather than a scene walk: nothing here takes a scene, a light list, or an object.
cy_add_module(
    NAME cy_rendering_shadows
    LAYER rendering
    ALIAS cy::rendering-shadows
    TYPE STATIC
    SOURCES
        src/address_space.cpp
        src/bias.cpp
        src/budget.cpp
        src/cache.cpp
        src/clipmap.cpp
        src/fallback.cpp
        src/invalidation.cpp
        src/pages.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
