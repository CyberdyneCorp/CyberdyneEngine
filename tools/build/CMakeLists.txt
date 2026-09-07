# tools/build/ — layer 7: the derivation graph and the build service. M6 tasks 7.1–7.6.
#
# `build-and-packaging` at Working. Five milestones were built by a script that is correct because it
# is re-run from scratch; this is the graph that replaces it — derivation keys, explicit inputs,
# immutable artefacts, the derived data cache, the build service, precise invalidation, packages and
# chunk-level patching.
#
# TWO TARGETS, FOR THE REASON tools/cook/ HAS TWO. `cy_build_graph` is a library with no `main` and
# no argument parsing; `cy_build` is a thin front end over it. The interesting assertions — which
# nodes ran, which hit the cache, whether two builds produced the same bytes, whether an interrupted
# patch left the previous build playable — are on the report and on the store, not on an exit code.
#
# LAYER tools, which may link any layer: a tool consumes the engine rather than sitting in it. None
# of this is linked into a shipped runtime.

# --- The toolchain fingerprint (design.md §1.3) ---------------------------------------------------
#
# The correctness bug M6's spike was commissioned to find: `import_derivation_key` contributes no
# compiler, no flags and no library versions, so two builds of one importer at -O2 and -O0 computed
# the identical key and one was served the other's artefact with a reported cache hit.
#
# The remedy has to be GENERATED rather than written, because a hand-maintained list of library
# versions goes stale the first time somebody adds a dependency in a hurry — which is the same
# failure as the hand-maintained importer version the spike found. So the two dependency manifests
# are read here, at configure time, and become a header. Adding a dependency without adding it to
# every derivation key is then impossible rather than discouraged, and task 0b's attribution gate and
# this key model are one mechanism seen from two ends.

function(_cy_build_read_manifest path table_key out_var)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR
            "tools/build: ${path} does not exist, so the toolchain fingerprint cannot be generated.\n"
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
            "tools/build: no entries were parsed out of ${path}.\n"
            "  The fingerprint would then be silently empty, which is the state design.md §1.3 "
            "found on the real importer arriving through a different door. If the manifest's format "
            "changed, this parser changes with it.")
    endif()
    # Sorted, so that reordering a manifest does not change every derivation key in the project.
    list(SORT entries)
    list(JOIN entries ";" joined)
    set(${out_var} "${joined}" PARENT_SCOPE)
endfunction()

_cy_build_read_manifest("${CMAKE_SOURCE_DIR}/deps/manifest.toml" dependency _cy_build_libraries)
_cy_build_read_manifest("${CMAKE_SOURCE_DIR}/deps/host-tools.toml" host_tool _cy_build_host_tools)

set(CY_BUILD_TOOLCHAIN_COMPILER "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
set(CY_BUILD_TOOLCHAIN_TARGET "${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
set(CY_BUILD_TOOLCHAIN_LIBRARIES "${_cy_build_libraries};${_cy_build_host_tools}")

# The flags that can move a float, plus the language standard. Deliberately the configuration's own
# flags rather than every flag on the command line: the warning set does not change an artefact and
# putting it in the key would make a key too fine, which §1.6 measures as a cache that leaks.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _cy_build_config)
set(CY_BUILD_TOOLCHAIN_FLAGS
    "c++${CMAKE_CXX_STANDARD} ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_cy_build_config}}")
string(STRIP "${CY_BUILD_TOOLCHAIN_FLAGS}" CY_BUILD_TOOLCHAIN_FLAGS)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/toolchain_generated.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/cy/build/toolchain_generated.h"
    @ONLY)

cy_add_module(
    NAME cy_build_graph
    LAYER tools
    ALIAS cy::build-graph
    TYPE STATIC
    SOURCES
        src/toolchain.cpp
        src/graph.cpp
        src/key.cpp
        src/artefact_store.cpp
        src/producer.cpp
        src/service.cpp
        src/package.cpp
        src/patch.cpp
        src/description.cpp
        src/text.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}/generated"
    PUBLIC_DEPENDENCIES cy::core-assets cy::core-memory)

find_package(Threads REQUIRED)
target_link_libraries(cy_build_graph PUBLIC Threads::Threads)

cy_add_module(
    NAME cy_build
    LAYER tools
    TYPE EXECUTABLE
    SOURCES
        src/main.cpp
    PRIVATE_DEPENDENCIES cy::build-graph)

# tests/ is read now and declares its suites later; see the comment at the top of it for why.
if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
