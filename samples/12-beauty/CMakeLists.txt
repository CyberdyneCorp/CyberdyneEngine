# samples/12-beauty — M11.c's closing artefact: the art-directed beauty shot.
#
# ================================================================================================
# WHAT THIS TARGET'S DEPENDENCY LIST IS FOR
# ================================================================================================
#
# M11.c's spike walked the rung's path — author, compile, encode, bind, assemble, capture — and found
# junction 1 REFUSED, junction 3 PARTIAL and junctions 4 and 5 ABSENT. This one target links the
# whole of what closes them:
#
#   cy::import              the cook. PNG in through M11.c's own DEFLATE decoder, BC7 and BC5 blocks
#                           with a mip chain out through M11.c's own encoder, and the `.cyprim`
#                           generator the geometry is built with.
#   cy::rendering-assembly  the frame. The post chain decides the feature set, the exposure and the
#                           tonemap run, and the stage list the frame ran is published beside the
#                           still by a manifest built from the frame's own report.
#   cy::rendering-pipeline  the tonemapping resolve — `cy/fullscreen.slang`'s own, the same one every
#                           other caller of the layer binds.
#   cy::rendering-sky       the sky. The engine's atmosphere, its multiple-scattering table and its
#                           volumetric cloud march, and `compose_sky_lighting` for the sun's
#                           illuminance and the sky's irradiance.
#
# WHAT IS DELIBERATELY NOT HERE. `cy::rendering-gi` — no global illumination pass runs and the
# manifest says so. `cy::vfx` — `vfx-system` is Working, not Complete, and a shot with sprites in it
# would have made none of its four recorded absences false. `cy::rendering-virtual-geometry` — the
# geometry is six primitives and a cluster hierarchy over them would be a claim about a mechanism
# this picture does not exercise.
#
# ================================================================================================
# THE COMPILED MATERIALS ARE NOT LINKED IN AND THAT IS THE POINT
# ================================================================================================
#
# The three fragment programs this sample binds are compiled AT CAPTURE TIME, by
# `just capture-beauty-shot`, out of the `.cygraph` files the editor's canvas authored:
#
#   cy-author-material   -> content/beauty/materials/*.cymatcanvas   (the editor's canvas)
#   cy_material author   -> *.cygraph, *.slang, *.cymatinfo          (the engine's lowering)
#   slangc               -> *.spv                                    (the shader pipeline)
#   cy_sample_beauty     -> the picture
#
# A SPIR-V header embedded here would make the picture a function of the last person who ran the
# embed script. Loading the modules at run time makes it a function of the committed graphs.
#
# ================================================================================================
# THERE IS NO CTest ENTRY, AND THAT IS DELIBERATE
# ================================================================================================
#
# The picture needs a graphics device, and `just/run.just`'s rule for `capture-virtual-geometry`
# applies unchanged: "on a machine without one the tool says so and writes nothing, which is why this
# is a recipe a person runs and not a test." What IS gated automatically is everything underneath —
# `unit.import`'s codec and encoder cases, `unit.graph_material`'s three front ends,
# `unit.material_compiler`, `unit.assembly` and the manifest's own suite.

cy_add_module(
    NAME cy_sample_beauty
    LAYER tools
    TYPE EXECUTABLE
    SOURCES
        main.cpp
        shot.cpp
        stage.cpp
        "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
    PRIVATE_DEPENDENCIES
        cy::import
        cy::rendering-assembly
        cy::rendering-graph
        cy::rendering-pipeline
        cy::rendering-sky
        cy::rhi
        cy::rhi-null)

target_include_directories(cy_sample_beauty
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                   "${CMAKE_SOURCE_DIR}/tests/render")

if(CY_RENDERER_VULKAN)
    target_link_libraries(cy_sample_beauty PRIVATE cy::rhi-vulkan)
    target_compile_definitions(cy_sample_beauty PRIVATE CY_SAMPLE_BEAUTY_VULKAN=1)
endif()
