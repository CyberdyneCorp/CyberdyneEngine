# src/core/assets/ — layer 0: asset identity, the virtual filesystem, the package read path, and
# asynchronous loading over the job system.
#
# Section 3.3. Governed by `core-assets-and-io`, which reaches Seed at M1 and no further: cooking is
# M2 and streaming under a residency policy is M6. Compression goes through the engine-owned
# interface over the pinned codec, never a third-party type in a public header.
#
# Loading is asynchronous over cy::core-jobs and never blocks a worker on I/O.
#
# THE DEPENDENCIES, AND WHY EACH IS WHERE IT IS.
#   cy::core-base      public   Expected, Error, the fixed-width types. Every header uses them.
#   cy::core-memory    public   Array, Span, Ref, UniquePtr, Allocator appear in public headers.
#   cy::core-values    public   AssetId is the values module's type (task 1.3.2), by design.
#   cy::core-jobs      public   AssetSystem::start takes a JobSystem and an AsyncService.
#   cy::core-reflect   public   the text and binary serialization forms describe reflected data.
#   cy::core-diagnostics private  the counters go onto the M0 trace; no public header includes it.
#   cy::dep::zstd      private  named by src/compression.cpp and by nothing else.
#   cy::dep::blake3    private  named by src/hash.cpp and by nothing else.
#
# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_core_assets
    LAYER core
    ALIAS cy::core-assets
    TYPE STATIC
    SOURCES
        src/asset_system.cpp
        src/compression.cpp
        src/diagnostics.cpp
        src/file.cpp
        src/hash.cpp
        src/identity.cpp
        src/package_manifest.cpp
        src/package_mount.cpp
        src/package_reader.cpp
        src/package_writer.cpp
        src/path.cpp
        src/serialization.cpp
        src/vfs.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::core-values cy::core-jobs cy::core-reflect
    PRIVATE_DEPENDENCIES cy::core-diagnostics cy::dep::zstd cy::dep::blake3)

# tests/ is read now and declares its suites later; see the comment at the top of it for why.
if(CY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
