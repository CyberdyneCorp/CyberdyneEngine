# src/servers/render/culling/ — layer 2: GPU-driven culling's records, and the reference that says
# what a dispatch must compute. M6 task 8.5.
#
# `rendering-culling-and-lod` reaches **Working** at M6, and the two requirements it was missing are
# here: GPU-driven culling and occlusion culling over a hierarchical depth buffer. The CPU half of
# the capability — spatial indexing, frustum culling, discrete LOD, visibility ranges, HLOD and
# shadow-caster culling — landed at Seed in src/rendering/culling/ and is untouched.
#
# WHY THE TWO ARE IN DIFFERENT PLACES, WHICH IS THE FIRST QUESTION A READER WILL HAVE. They sit at
# different layers because they hold different things. src/rendering/culling/ is layer 4: it holds a
# `SpatialIndex` of `DynamicBvh` trees, it is handed a `jobs::JobSystem`, and it produces CPU-side
# visible lists for a frame that is being assembled above it. What is HERE is the data a compute
# dispatch reads and writes — the view constants, the indirect draw arguments, the counters, the
# depth pyramid's shape — and every one of those is a LAYOUT the render server publishes and the
# backends consume. `gpu_scene.h` is at layer 2 for exactly the same reason and this module reads it
# directly; a GPU cull whose input record lived a layer above its consumer would be the wrong way up.
#
# NO DEVICE, NO SHADER, NO TEXTURE. Layer 2 may not name one, and the consequence is the useful part:
# `cpu_reference_cull` runs the whole algorithm headless, so the compute shader that lands at M7 is
# checked against a reference rather than against a screenshot. tests/ runs in every profile on a
# machine with no GPU.
cy_add_module(
    NAME cy_servers_render_culling
    LAYER servers
    ALIAS cy::servers-render-culling
    TYPE STATIC
    SOURCES
        src/gpu_cull.cpp
        src/hzb.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::servers-render)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
