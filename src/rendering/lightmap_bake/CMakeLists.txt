# src/rendering/lightmap_bake/ — layer 4: the lightmap bake and the cooked lightmap.
#
# `rendering-global-illumination` — "Lightmap baking", "UV2 and chart packing". Shared atlas pages
# with bilinear and mip padding, a triangle tracer behind gi::SceneTracer so gi::PathTracer bakes
# every texel, denoising through denoise::Denoiser, border dilation, seam reconciliation, the
# dynamic caches' seeds from the same run, the cooked payload, and the CPU reference sampler
# cy/frame.slang's lightmap term is held to.
#
# NO DEVICE, as cy::rendering-gi has none: every case in tests/ runs headless. The device half — the
# atlas as textures and the frame words that address it — is src/rendering/lightmaps/.
cy_add_module(
    NAME cy_rendering_lightmap_bake
    LAYER rendering
    ALIAS cy::rendering-lightmap-bake
    TYPE STATIC
    SOURCES
        src/asset.cpp
        src/atlas.cpp
        src/bake.cpp
        src/postprocess.cpp
        src/raster.cpp
        src/scene.cpp
        src/trace.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-gi
    PRIVATE_DEPENDENCIES cy::rendering-denoise)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
