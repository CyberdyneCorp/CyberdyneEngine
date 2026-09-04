# src/backends/shader/slang/ — layer 3: the Slang front end.
#
# The only directory in the engine that names a Slang type. Gated by CY_SHADER_SLANG, which also
# gates the fetch of Slang in deps/manifest.toml — a disabled front end downloads nothing.
#
# It links cy::shader rather than being part of it, for the same reason cy_rhi_vulkan is a separate
# target from cy_rhi: a build with the option off then contains no Slang code at all, rather than
# Slang code that is never called, and the layer checker can state "no Slang header outside
# src/backends/" over a target that either exists or does not.
cy_add_module(
    NAME cy_shader_slang
    LAYER backends
    ALIAS cy::shader-slang
    TYPE STATIC
    SOURCES
        src/slang_compiler.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::shader
    # cy::dep::slang is private: no Slang type reaches this target's public header, which is the
    # whole point of the seam. `slang.h` is named by src/slang_compiler.cpp and by nothing else.
    PRIVATE_DEPENDENCIES cy::dep::slang)
