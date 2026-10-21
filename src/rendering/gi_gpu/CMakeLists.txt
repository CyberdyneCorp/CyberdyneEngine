# src/rendering/gi_gpu/ — layer 4: CyberGI on the device. Issue #35, stages 1 to 5.
#
# `rendering-global-illumination` — "Surface cache" and "Distance field representation", on a
# device: the GI scene's distance-field clipmaps and surface cards uploaded incrementally (stage 1),
# the surface cache's card lighting in compute (stage 2), and the radiance cache's gather and query
# with the screen, software and hardware tracing tiers (stage 3). The algorithms, and the oracle every
# dispatch here is checked against, are cy::rendering-gi, which stays device-free; this module is
# the one place the two meet, for the reason src/rendering/light_probes/ is a module of its own.
#
# BEHIND A SEAM, NOT BESIDE THE HOST CACHE. `GpuSurfaceShading` is a `gi::SurfaceShadingBackend`:
# `IlluminationSystem::set_surface_shading` installs it, and nothing in cy::rendering-gi names it.
#
# IN THE FRAME SINCE STAGE 4. `GpuScreenResolve` is the producer of the forward frame's
# `ScreenSpaceGi` stage (cy::rendering-forward's `FrameStageDeclaration`, which is the one reason
# this module links it): the per-pixel resolve and its denoiser, published as the two textures
# `cy/frame.slang` composites where the irradiance volume's ambient sits. A frame that does not
# declare the stage reads nothing this module writes. `GpuGiFrame` is that attachment as one frame
# feature, with the GI's debug views drawn on the device (`GpuGiDebugView`) — what the editor's
# viewport and a game's host attach.
#
# THE SPIR-V AND MSL ARE CHECKED IN, not compiled by the build, for the reason
# src/rendering/skinning/CMakeLists.txt gives. shaders/regenerate.py rewrites src/gi_gpu_spirv.h and
# src/gi_gpu_msl.h.
# Until regenerate.py has produced both headers, the module is not configured at all: gpu_scene.cpp
# includes them, and a default build must not stop on a missing file. Nothing else links it yet.
foreach(_cy_gi_gpu_header gi_gpu_spirv.h gi_gpu_msl.h)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/${_cy_gi_gpu_header}")
        message(STATUS "cy::rendering-gi-gpu skipped: src/${_cy_gi_gpu_header} not generated yet "
                       "(run shaders/regenerate.py)")
        return()
    endif()
endforeach()

cy_add_module(
    NAME cy_rendering_gi_gpu
    LAYER rendering
    ALIAS cy::rendering-gi-gpu
    TYPE STATIC
    SOURCES src/gpu_scene.cpp src/gpu_surface_cache.cpp src/gpu_radiance_cache.cpp
            src/gpu_resolve.cpp src/gpu_budget.cpp src/gpu_debug_view.cpp src/gpu_frame.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    # cy::rendering-raytracing-gpu for the hardware tier: the structures a ray query traverses are
    # the acceleration service's, built on the device by that module and never by this one.
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-gi
                        cy::rendering-graph cy::rendering-raytracing cy::rendering-raytracing-gpu
                        cy::rendering-forward cy::rendering-arbiter cy::rhi)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
