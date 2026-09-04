# src/rendering/shaders/ — the engine's own Slang source: the shader standard library, and the
# shaders the frame is built from. Task 3.1.
#
# `shader-system` requires the engine to provide "a shader standard library of Slang modules: BRDF
# functions, light evaluation, cluster lookup, shadow sampling, GI sampling, tonemapping, colour
# space conversion, packing and encoding helpers, noise, and sampling patterns". That is what
# cy/ holds, one module per subject, imported by name — `import cy.brdf` — with no textual
# preprocessor inclusion anywhere, which the specification asks for outright.
#
# THIS DIRECTORY COMPILES NO C++. The target below is INTERFACE and carries a build step that stages
# the tree into ${CMAKE_BINARY_DIR}/shaders/, so that a test, a tool or the sample can mount that
# directory through cy::assets::VirtualFileSystem and reach the modules by the same virtual paths a
# shipped build will. Staging rather than reading from the source tree is what keeps the runtime's
# view of "where shaders are" identical in a development build and a packaged one.
#
# THE FILES ARE LISTED EXPLICITLY, for the same reason source files are: a module this list does not
# name is staged by nothing, and an `import` of it fails at compile time rather than silently
# resolving to a stale copy somebody left in the build directory.
set(CY_SHADER_STDLIB_MODULES
    cy/brdf.slang
    cy/cluster.slang
    cy/color.slang
    cy/fullscreen.slang
    cy/globals.slang
    cy/light.slang
    cy/material.slang
    cy/noise.slang
    cy/packing.slang
    cy/sampling.slang
    cy/shadow.slang
    cy/tonemap.slang
    cy/view.slang)

set(cy_shader_stage_dir "${CMAKE_BINARY_DIR}/shaders")
set(cy_shader_staged "")
foreach(module IN LISTS CY_SHADER_STDLIB_MODULES)
    add_custom_command(
        OUTPUT "${cy_shader_stage_dir}/${module}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/${module}" "${cy_shader_stage_dir}/${module}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${module}"
        COMMENT "Staging shader module ${module}"
        VERBATIM)
    list(APPEND cy_shader_staged "${cy_shader_stage_dir}/${module}")
endforeach()

add_custom_target(cy_shaders_stage ALL DEPENDS ${cy_shader_staged}
                  SOURCES ${CY_SHADER_STDLIB_MODULES})

# The engine-facing handle. INTERFACE because there is nothing to compile; anything that links it
# is guaranteed the staged tree exists before it runs.
cy_add_module(
    NAME cy_shaders
    LAYER rendering
    ALIAS cy::shaders
    TYPE INTERFACE)
add_dependencies(cy_shaders cy_shaders_stage)

# Where the staged tree is, for a test or a tool that mounts it. A cache variable rather than a
# compile definition: a path baked into every consumer's command line would be a path in a shipped
# binary, and the runtime finds its shaders through the virtual filesystem instead.
set(CY_SHADER_STAGE_DIR "${cy_shader_stage_dir}"
    CACHE INTERNAL "Where the engine's Slang modules are staged")
