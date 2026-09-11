# src/rendering/particles/ — layer 4: the pipeline layer's first consumer. M8.c task 1b.4.
#
# Task 1b.4: "A particle renderer is the first consumer, and it proves the layer by USING it rather
# than by a test written beside it."
#
# So this target's dependency list is the claim. It links cy::rendering-pipeline and reaches the
# frame through `PassExtension`; it does NOT link cy::rendering-graph, cy::rendering-assembly or
# cy::rendering-forward, because a consumer that had to declare its own passes would be evidence the
# layer's seam is in the wrong place. It reuses the frame's own descriptor set layouts rather than
# declaring copies of them — see particle_renderer.cpp — which is what makes its pipeline layout
# compatible with the frame's and the already-bound sets stay bound.
#
# WHAT IS NOT HERE. The simulation. `vfx-system` owns emitters, the graph compiler, the attribute
# layout and the scheduler, and `src/vfx/` is where they land; the seam between the two is
# `ParticleInstance`, which is a position, a size and a colour.
cy_add_module(
    NAME cy_rendering_particles
    LAYER rendering
    ALIAS cy::rendering-particles
    TYPE STATIC
    SOURCES src/particle_renderer.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    PUBLIC_DEPENDENCIES
        cy::core-base
        cy::core-math
        cy::rendering-pipeline
        cy::rhi)

add_dependencies(cy_rendering_particles cy_shaders_stage)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
