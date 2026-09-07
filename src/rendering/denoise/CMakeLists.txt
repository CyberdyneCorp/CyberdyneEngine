# src/rendering/denoise/ — layer 4: the ONE reconstruction every stochastic signal goes through.
#
# `denoising` at Working for M7, task 9.3. Temporal accumulation over a reprojected history,
# variance estimation, an edge-aware a-trous cascade driven by variance and sample count, history
# validation, the quality ladder the GI budget pulls on, and the per-signal diagnostics.
#
# ONE FILTER, AND THE ENFORCEMENT IS STRUCTURAL. `SignalKind` selects a `SignalConfig` and a history
# slot and is read nowhere else; everything that differs between indirect diffuse, indirect
# specular, ray-traced shadows, ambient occlusion and stochastic direct lighting is a field of that
# config. tests/test_one_filter.cpp is the check: the same buffer denoised under two signal kinds
# carrying the same configuration comes out bit for bit identical.
#
# NO DEVICE, NO GRAPH, NO SHADER, AND NO TEMPORAL FRAMEWORK. The reprojection is an INPUT
# (`HistoryGuidance`) rather than a dependency: `denoising` requires that this module not implement
# its own history handling, and `temporal-rendering` is written in the same milestone by another
# hand — a struct of spans is the interface either can fill, and it is what keeps every case in
# tests/ headless.
#
# cy::core-jobs is here for one function, `jobs::monotonic_now_ns()`, which stamps the per-signal
# cost the diagnostics requirement asks for.
cy_add_module(
    NAME cy_rendering_denoise
    LAYER rendering
    ALIAS cy::rendering-denoise
    TYPE STATIC
    SOURCES
        src/denoiser.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory
    PRIVATE_DEPENDENCIES cy::core-jobs)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
