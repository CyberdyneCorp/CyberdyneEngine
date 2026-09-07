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

# --- The toolchain fingerprint moved to layer 0 (M7 task 1.1) -------------------------------------
#
# M6 generated `cy/build/toolchain_generated.h` here, from deps/manifest.toml and
# deps/host-tools.toml. It was the right mechanism at the wrong layer: `cy::import` and
# `cy::shader` cannot link a tool, so the two producers that actually cook content could not
# contribute a toolchain to their keys and M6's closing gate re-measured the original defect —
# two importer binaries, one cache, 1 hit and 0 miss.
#
# The generation now lives in src/core/assets/CMakeLists.txt beside the derivation key, and
# `cy/build/toolchain.h` is the tools-layer face of `cy::assets::ToolchainFingerprint`.

cy_add_module(
    NAME cy_build_graph
    LAYER tools
    ALIAS cy::build-graph
    TYPE STATIC
    SOURCES
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
    PUBLIC_DEPENDENCIES cy::core-assets cy::core-memory)

find_package(Threads REQUIRED)
target_link_libraries(cy_build_graph PUBLIC Threads::Threads)

# --- The real cooks, as graph nodes (M7 task 1.4) -------------------------------------------------
#
# A SEPARATE TARGET FROM cy_build_graph, and the split is the point. The graph library links
# cy::core-assets and cy::core-memory and nothing else; the importer links ufbx and xatlas and the
# cook links the ECS, the scene serializer and the reflection registry. Folding either into the
# graph would make every consumer of a derivation key pay for a glTF parser. This is the join, at
# the same layer, and only `cy_build` links it.
cy_add_module(
    NAME cy_build_content
    LAYER tools
    ALIAS cy::build-content
    TYPE STATIC
    SOURCES
        src/content_producers.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::build-graph cy::import cy::cook-pipeline)

cy_add_module(
    NAME cy_build
    LAYER tools
    TYPE EXECUTABLE
    SOURCES
        src/main.cpp
    PRIVATE_DEPENDENCIES cy::build-graph cy::build-content)

# tests/ is read now and declares its suites later; see the comment at the top of it for why.
if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
