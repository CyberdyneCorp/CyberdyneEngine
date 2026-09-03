# src/core/assets/ — layer 0: asset identity, the virtual filesystem, the package read path, and
# asynchronous loading over the job system.
#
# Section 3.3. Governed by `core-assets-and-io`, which reaches Seed at M1 and no further: cooking is
# M2 and streaming under a residency policy is M6. Compression goes through the engine-owned
# interface over the pinned codec, never a third-party type in a public header.
#
# Loading is asynchronous over cy::core-jobs and never blocks a worker on I/O.

# One placeholder source, replaced by the first real one. Sources are listed explicitly and never
# globbed: a file this list does not name is compiled by nothing, which reads in a review exactly
# like a file that passes.
cy_add_module(
    NAME cy_core_assets
    LAYER core
    ALIAS cy::core-assets
    TYPE STATIC
    SOURCES
        src/placeholder.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base)
