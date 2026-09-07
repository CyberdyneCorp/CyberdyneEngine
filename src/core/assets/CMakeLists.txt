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

# --- The toolchain fingerprint (M6 design.md §1.3, M7 task 1.1) -----------------------------------
#
# The correctness bug M6's spike was commissioned to find: `import_derivation_key` contributes no
# compiler, no flags and no library versions, so two builds of one importer at -O2 and -O0 computed
# the identical key and one was served the other's artefact with a reported cache hit.
#
# M6 generated this header under tools/build/, which is layer 7 — out of reach of the importer's own
# module and of the shader cache at the backends layer, so the function that actually cooks content
# still could not contribute a toolchain. It is generated HERE, beside the derivation key it feeds,
# because `asset-import-pipeline` and `build-and-packaging` both require one cache over all derived
# data and one cache needs one key.
#
# It has to be GENERATED rather than written, because a hand-maintained list of library versions goes
# stale the first time somebody adds a dependency in a hurry — the same failure as the
# hand-maintained importer version the spike found. So the two dependency manifests are read here, at
# configure time, and become a header. Adding a dependency without adding it to every derivation key
# is then impossible rather than discouraged.

function(_cy_assets_read_manifest path table_key out_var)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR
            "src/core/assets: ${path} does not exist, so the toolchain fingerprint cannot be "
            "generated.\n"
            "  Every derivation key contributes the version of every third-party library linked "
            "into a producer, and that list comes from the dependency manifests and from nowhere "
            "else. A build whose keys ignore its libraries serves artefacts built by a different "
            "zstd — see openspec/changes/implement-m6-scale/design.md §1.3.")
    endif()

    file(STRINGS "${path}" lines)
    set(entries "")
    set(pending_name "")
    foreach(line IN LISTS lines)
        string(STRIP "${line}" line)
        if(line STREQUAL "[[${table_key}]]")
            set(pending_name "")
        elseif(line MATCHES "^name[ \t]*=[ \t]*\"([^\"]*)\"$")
            set(pending_name "${CMAKE_MATCH_1}")
        elseif(line MATCHES "^version[ \t]*=[ \t]*\"([^\"]*)\"$" AND NOT pending_name STREQUAL "")
            set(entry "${pending_name}=${CMAKE_MATCH_1}")
            # A dependency the project may take from the system is TWO different libraries wearing
            # one version number. zstd 1.5.7 (the pin) and zstd 1.5.5 (this host) compress the same
            # input to different bytes, and CY_SYSTEM_ZSTD picks between them, so which one was used
            # has to be in the key.
            string(TOUPPER "${pending_name}" upper)
            if(DEFINED CY_SYSTEM_${upper} AND CY_SYSTEM_${upper})
                set(entry "${entry}(system)")
            endif()
            list(APPEND entries "${entry}")
            set(pending_name "")
        endif()
    endforeach()

    if(entries STREQUAL "")
        message(FATAL_ERROR
            "src/core/assets: no entries were parsed out of ${path}.\n"
            "  The fingerprint would then be silently empty, which is the state design.md §1.3 "
            "found on the real importer arriving through a different door. If the manifest's format "
            "changed, this parser changes with it.")
    endif()
    # Sorted, so that reordering a manifest does not change every derivation key in the project.
    list(SORT entries)
    list(JOIN entries ";" joined)
    set(${out_var} "${joined}" PARENT_SCOPE)
endfunction()

_cy_assets_read_manifest("${CMAKE_SOURCE_DIR}/deps/manifest.toml" dependency _cy_assets_libraries)
_cy_assets_read_manifest("${CMAKE_SOURCE_DIR}/deps/host-tools.toml" host_tool _cy_assets_host_tools)

set(CY_TOOLCHAIN_COMPILER "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
set(CY_TOOLCHAIN_TARGET "${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
set(CY_TOOLCHAIN_LIBRARIES "${_cy_assets_libraries};${_cy_assets_host_tools}")

# The flags that can move a float, plus the language standard. Deliberately the configuration's own
# flags rather than every flag on the command line: the warning set does not change an artefact and
# putting it in the key would make a key too fine, which M6 design.md §1.6 measures as a cache that
# leaks.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _cy_assets_config)
set(CY_TOOLCHAIN_FLAGS
    "c++${CMAKE_CXX_STANDARD} ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_cy_assets_config}}")
string(STRIP "${CY_TOOLCHAIN_FLAGS}" CY_TOOLCHAIN_FLAGS)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/toolchain_generated.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/cy/core/assets/toolchain_generated.h"
    @ONLY)

cy_add_module(
    NAME cy_core_assets
    LAYER core
    ALIAS cy::core-assets
    TYPE STATIC
    SOURCES
        src/asset_system.cpp
        src/compression.cpp
        src/cooked.cpp
        src/derivation.cpp
        src/derived_cache.cpp
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
        src/streaming.cpp
        src/toolchain.cpp
        src/vfs.cpp
        src/watch.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
                         "${CMAKE_CURRENT_BINARY_DIR}/generated"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::core-values cy::core-jobs cy::core-reflect
    PRIVATE_DEPENDENCIES cy::core-diagnostics cy::dep::zstd cy::dep::blake3)

# tests/ is read now and declares its suites later; see the comment at the top of it for why.
if(CY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
