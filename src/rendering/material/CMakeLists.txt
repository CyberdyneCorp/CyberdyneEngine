# src/rendering/material/ — layer 4: the BRDF, the material model, and material validation.
# Tasks 4.2.3, 4.2.4 and 4.2.5.
#
# `rendering-materials-and-shading` specifies the BRDF concretely — exact terms and their sources —
# "because 'PBR' alone is not a specification and mismatched terms produce subtly wrong lighting
# that is very hard to debug later". This module is the CPU-side statement of those terms, and the
# reason it exists as C++ rather than only as Slang is that a shading term nobody can call from a
# test is a shading term whose energy conservation is checked by looking at a screenshot.
#
# WHY LAYER 4 AND NOT LAYER 2, WHEN MATERIALS ARE A RENDER SERVER OBJECT FAMILY. The server holds
# what SORTING and DRAWING need of a material — the program to bind, the table index, the blend mode
# (`render::MaterialRecord`). What is here is the model: the parameter layout, the GPU material
# table's bytes, the standard material's slots, the fallbacks, and validation. The split is the same
# one `rendering-architecture` draws between the server's handle model and the renderer's own types,
# and it is what keeps the render server free of a shading model's arithmetic.
#
# NO DEVICE AND NO SHADER COMPILER. This module allocates no buffer and compiles nothing: the table
# is bytes, and whoever owns a device uploads them. That is what lets every case in tests/ run with
# no GPU — including the DFG table bake, which is the one piece of the image-based lighting chain
# that would otherwise need one.

# --- M7: the material compiler (tasks 6.1-6.4) ----------------------------------------------------
#
# `material-compiler` at Working: graph and text front-ends, a content-hashed typed IR, the
# optimisation pipeline, closure lowering, the program family, quality tiers, Slang emission, cost
# analysis with per-node attribution, and material classification and binning.
#
# IN THIS TARGET RATHER THAN A NEW ONE. The compiler produces exactly what this module already
# defines — a `MaterialProgram` parameter layout for the GPU material table — and material.h has
# said since M3 that "a material compiled from a closure graph at M7 fills the same shape, and
# nothing downstream changes". A separate target would have to depend on this one for that type and
# be depended on by it for nothing, which is a split with a direction and no boundary.
#
# STILL NO DEVICE AND STILL NO SHADER COMPILER. The compiler emits Slang TEXT; `shader-system` owns
# compiling it, and `src/backends/shader/` is layer 3. That is what keeps every property in this
# module's suite checkable with no GPU, including the exit criterion "a graph and a hand-written
# material produce identical programs" — which is a property of the text.
cy_add_module(
    NAME cy_rendering_material
    LAYER rendering
    ALIAS cy::rendering-material
    TYPE STATIC
    SOURCES
        src/binning.cpp
        src/brdf.cpp
        src/compiler.cpp
        src/cost.cpp
        src/emit.cpp
        src/graph.cpp
        src/ir.cpp
        src/lowering.cpp
        src/material.cpp
        src/passes.cpp
        src/standard.cpp
        src/text.cpp
        src/validation.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    # cy::servers-render is layer 2 and supplies the value vocabulary both halves share —
    # `BlendMode`, `ShadingModel`, `TextureFormat`. Depending downward on the server rather than
    # restating those enumerations here is what keeps the sort key's spelling of a blend mode and
    # the material's the same one.
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::core-values
                        cy::servers-render)

# --- The step that makes a generated material COMPILE. M8.c task 1b.3 --------------------------
#
# M7 recorded the gap and it was still open on the tree M8.b closed: the bundle carries the IR, the
# generated source, the cost report and the cook key, and NOTHING invokes the compiler. Handing the
# emitted Slang to slangc against src/rendering/shaders/ failed on the first identifier — the
# closure vocabulary did not exist, and a generated program declares none of the material's own
# parameters, attributes or texture slots.
#
# A SECOND TARGET RATHER THAN A SOURCE IN THE FIRST, and the reason is a cook key. `docs/ROADMAP.md`
# records that "`src/rendering/material/` is not modified — it is M7's closed work and one extension
# invalidates its cook keys": `cy_rendering_material`'s source list above is untouched, `emit.cpp`
# is untouched, and every byte the emitter produces is unchanged. The prelude is written AROUND that
# output and never into it.
#
# It depends on cy::shader (layer 3) because THIS is where the engine invokes the compiler, which is
# the sentence M7's gate said was false. cy::rendering-material does NOT depend on it, so nothing
# that only wants the IR pays for the shader pipeline.
cy_add_module(
    NAME cy_rendering_material_slang
    LAYER rendering
    ALIAS cy::rendering-material-slang
    TYPE STATIC
    SOURCES src/slang_program.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::rendering-material)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
