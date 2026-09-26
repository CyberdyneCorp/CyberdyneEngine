# tools/shaders/ — layer 7: the shader toolchain's front end. M11.c tasks 1.5-1.7.
#
# ONLY BUILT WITH CY_SHADER_SLANG, and the reason is the same one that keeps src/backends/shader/
# split in two: `shader-system` requires a shipping build to contain no Slang compiler, so the front
# end is excluded from the link rather than stubbed. A tool whose whole subject is compiling source
# has nothing to do in a configuration that cannot.
#
# TWO TARGETS, for the reason tools/material/, tools/cook/ and tools/import/ each have two. The
# interesting assertions — that the same graph compiled for two targets declares the same interface,
# that an artefact is in the form its target names — are on the report, not on an exit code, and a
# library is what a test can call.
if(NOT CY_SHADER_SLANG)
    return()
endif()

cy_add_module(
    NAME cy_shader_targets
    LAYER tools
    ALIAS cy::shader-targets
    TYPE STATIC
    SOURCES
        src/targets.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::shader cy::shader-slang cy::core-assets)

cy_add_module(
    NAME cy_shaderc
    LAYER tools
    TYPE EXECUTABLE
    SOURCES
        src/main.cpp
    PRIVATE_DEPENDENCIES cy::shader-targets)

# tests/ is read now and declares its suites later; see the comment at the top of it for why.
if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
