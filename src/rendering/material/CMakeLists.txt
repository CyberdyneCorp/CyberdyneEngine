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
cy_add_module(
    NAME cy_rendering_material
    LAYER rendering
    ALIAS cy::rendering-material
    TYPE STATIC
    SOURCES
        src/brdf.cpp
        src/material.cpp
        src/standard.cpp
        src/validation.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    # cy::servers-render is layer 2 and supplies the value vocabulary both halves share —
    # `BlendMode`, `ShadingModel`, `TextureFormat`. Depending downward on the server rather than
    # restating those enumerations here is what keeps the sort key's spelling of a blend mode and
    # the material's the same one.
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::core-values
                        cy::servers-render)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
