# src/backends/rhi/null/ — layer 3: the RHI without a GPU.
#
# Task 2.1.2, design.md §1. Written BEFORE the Vulkan backend, and that order is the design: with no
# Vulkan to lean on, the interface above it had to be an interface. Written afterwards it would have
# been a set of empty functions shaped by decisions Vulkan had already made.
#
# Always built, never gated. It is the last link in the backend fallback chain, and it is what makes
# a rendering test runnable in continuous integration on a machine with no device — which is most
# machines, and every one this project's CI has.
#
# It links nothing beyond the RHI itself. A dependency here on a graphics API would defeat the whole
# point of the target.
cy_add_module(
    NAME cy_rhi_null
    LAYER backends
    ALIAS cy::rhi-null
    TYPE STATIC
    SOURCES
        src/null_command_buffer.cpp
        src/null_device.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PUBLIC_DEPENDENCIES cy::rhi)
