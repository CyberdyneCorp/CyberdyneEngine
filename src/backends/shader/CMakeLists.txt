# src/backends/shader/ — layer 3: the shader system. Section 3 of the M3 tasks.
#
# Governed by `shader-system`, which reaches Working this milestone. Slang is the authoring language
# and SPIR-V the interchange form; THE ENGINE DOES NOT AUTHOR A SHADING LANGUAGE AND DOES NOT WRITE
# A SHADER OPTIMISER (`thirdparty-dependencies`). What is built here is the pipeline around somebody
# else's compiler: the source registry, the permutation model, reflection-driven binding, the
# tiered content-addressed cache, the shader library, hot reload, the global parameter block and
# pipeline state management.
#
# WHY LAYER 3 AND NOT LAYER 4. Reflection reads the SPIR-V binary, and a SPIR-V header is a graphics
# API header by tools/layercheck/layercheck.py's `gpuapi` rule — it may not appear above
# src/backends/. The Slang front end names Slang's own types for the same reason. Both belong here,
# beside the RHI, and everything above consumes `cy::shader`'s engine-owned vocabulary.
#
# THE TWO TARGETS, AND WHY THE SECOND IS OPTIONAL.
#
#   cy::shader        always built. Contains the SPIR-V passthrough front end, which is not a stub:
#                     `shader-system` requires that "a shipping build SHALL contain compiled
#                     backend-native shader artefacts and no Slang compiler", so consuming an
#                     already-compiled module is the shipping path and not a fallback. It is also
#                     what lets continuous integration exercise reflection, permutations, the cache,
#                     the library and hot reload on a machine with no shader toolchain.
#   cy::shader-slang  gated by CY_SHADER_SLANG, which also gates the fetch of Slang in
#                     deps/manifest.toml — a disabled front end downloads nothing.
#
# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_shader
    LAYER backends
    ALIAS cy::shader
    TYPE STATIC
    SOURCES
        src/cache.cpp
        src/compiler.cpp
        src/diagnostics.cpp
        src/globals.cpp
        src/hot_reload.cpp
        src/library.cpp
        src/permutation.cpp
        src/pipeline.cpp
        src/reflection.cpp
        src/source.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    # cy::rhi is public because reflection produces rhi::DescriptorBinding, rhi::PushConstantRange
    # and rhi::VertexAttribute directly — the whole point of task 3.3 is that no second description
    # of a layout exists to drift. cy::core-assets is public for VirtualPath, ContentHash and the
    # FileWatcher hot reload is built on.
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::core-values cy::core-assets cy::rhi)

# The Slang front end. Its own target so that a build with CY_SHADER_SLANG off contains no Slang
# code at all, rather than Slang code that is never called.
if(CY_SHADER_SLANG)
    add_subdirectory(slang)
endif()

# tests/ is read now and declares its suites later; see the comment at the top of it for why.
if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
