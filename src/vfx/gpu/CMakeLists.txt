# src/vfx/gpu/ — layer 4: the VFX compute dispatch. M10 task 5.1 and 5.2, `vfx-system`.
#
# ================================================================================================
# WHY THIS IS A THIRD TARGET AND NOT PART OF cy::vfx
# ================================================================================================
#
# `src/vfx/CMakeLists.txt` states the split its two targets make and the requirement behind it. This
# one adds the half neither may contain:
#
#   cy::vfx-compiler   may not name a device, because a compiler that needed one cannot run in a
#                      cook. It owns the binding contract — <cy/vfx/gpu_layout.h> — because the
#                      generator has to emit against it, and that header names no device either.
#
#   cy::vfx            may not name the render graph, the frame or a device. Its own README says
#                      why: "a simulation that had to know how a frame is assembled would be a
#                      second renderer, which is what this milestone exists to stop." The property
#                      that makes that claim checkable is that `cy::vfx`'s suites run headless on a
#                      machine with no GPU, and a device handle in the simulation world would end
#                      it.
#
#   cy::vfx-gpu        names all three: the device, the render graph and the shader front end. It
#                      links cy::vfx for `BudgetLevers` and cy::vfx-compiler for `CompiledSystem`
#                      and the binding contract, and NOTHING in either of them links back.
#
# That is `src/rendering/gpu_culling/`'s shape against `src/servers/render/culling/` and
# `src/rendering/skinning/`'s against `src/servers/render/geometry/`, for the same reason each of
# those records: a compute pass needs a device, the module that holds the algorithm may not name
# one, and a dispatch that can only be checked by looking at a picture is a dispatch nobody checks.
#
# ================================================================================================
# THE SPIR-V IS HALF CHECKED IN AND HALF COMPILED AT LOAD TIME, AND THE SPLIT IS NOT ARBITRARY
# ================================================================================================
#
# shaders/vfx_support.slang holds the three dispatches that are the SAME for every effect — the
# reset, the compaction that maintains every counter and writes the indirect arguments, and the
# sort. Those are compiled once by hand and embedded in src/vfx_support_spirv.h, exactly as
# src/rendering/gpu_culling/ and src/rendering/skinning/ do it and for the same reason: CY_SHADER_SLANG
# is off in Profile and Shipping, and a scheduler that only exists where Slang was built is a
# scheduler with two different frames.
#
# The SIMULATION KERNEL cannot be: it is generated from an authored graph and is different for every
# effect. `VfxGpuPass::create` compiles it through cy::shader's Slang front end where there is one
# and takes it pre-cooked where there is not — which is why cy::shader-slang is an OPTIONAL
# dependency here rather than a required one, and why the failure with neither is an error that
# names the missing front end.
cy_add_module(
    NAME cy_vfx_gpu
    LAYER rendering
    ALIAS cy::vfx-gpu
    TYPE STATIC
    SOURCES src/gpu_pass.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-graph cy::vfx
                        cy::vfx-compiler
    PRIVATE_DEPENDENCIES cy::rhi)

if(CY_SHADER_SLANG)
    target_compile_definitions(cy_vfx_gpu PRIVATE CY_SHADER_SLANG=1)
    target_link_libraries(cy_vfx_gpu PRIVATE cy::shader cy::shader-slang)
endif()

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
