# tools/import/ — layer 7: the asset import pipeline. M5 task 5.1.
#
# `asset-import-pipeline` reaches **Working** at M5: the importer framework, the cook cache,
# dependency tracking, glTF and texture import, and the mesh processing beneath them.
#
# WHY IT IS A TOOL AND NOT A SERVER. Nothing here runs in a shipped game. `thirdparty-dependencies`
# is explicit that source-format parsers are tool-time only — "WHEN a shipped runtime loads content
# THEN it SHALL read the cooked format directly, with no glTF or FBX parser present" — and the
# layer this module sits at is what makes that true rather than intended: layer 7 may link
# everything, and nothing links layer 7.
#
# TWO TARGETS, AND THE SPLIT IS WHY THE PIPELINE IS TESTABLE. `cy_import` is a library with no
# `main` and no argument parsing; `cy_import_cli` is a thin front end over it. The assertions worth
# making — which sub-assets a glTF produced, what the cache said and why, which diagnostic a
# mis-tagged normal map raised — are on the result and the report, not on an exit code. This is the
# same split tools/cook/ made for the same reason.
#
# WHAT THE COOK CACHE IS NOT DOING HERE. It is in src/core/assets/ at layer 0, because
# `asset-import-pipeline` requires ONE cache over all derived data and a cache that lived with the
# importer would be a second mechanism the day the shader toolchain wanted one. This module is a
# CLIENT of it, like every other producer will be.
#
# THE DEPENDENCIES, AND WHY EACH IS WHERE IT IS.
#   cy::core-assets    public   the derivation key, the cache, asset identity, the cooked header.
#   cy::core-base      public   Expected, Error, the fixed-width types.
#   cy::core-memory    public   Array and Span appear in public headers.
#   cy::core-values    public   cy::AssetId is the values module's type.
#   cy::core-math      public   Vec3, Vec4, the welder, the tangent generator and the atlas packer.
#
# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
#
# STEP 8 — ANIMATIONS — DEPENDS ON cy::animation, AND CONDITIONALLY. `animation-and-skinning`
# requires its runtime to be removable, and `-D CY_ANIMATION=OFF` removes src/animation/ from the
# build entirely: there is then no `Clip` to author and no codec to compress with. So the dependency
# and the definition that reveals it are added only when the runtime is there, and src/fbx_clip.cpp
# compiles either way — reporting that step 8 was not reached instead of pretending it was. The
# definition is PUBLIC because a test, and a tool that inspects a package, needs the same answer.
set(cy_import_animation_dependencies "")
set(cy_import_animation_definitions "")
if(CY_ANIMATION)
    set(cy_import_animation_dependencies cy::animation)
    # The keyword travels with its value, so that an empty list expands to nothing at all rather
    # than to a keyword cy_add_module() would refuse for having been given no value.
    set(cy_import_animation_definitions PUBLIC_DEFINITIONS CY_IMPORT_ANIMATION=1)
endif()

cy_add_module(
    NAME cy_import
    LAYER tools
    ALIAS cy::import
    TYPE STATIC
    SOURCES
        src/fbx.cpp
        src/fbx_clip.cpp
        src/fbx_skeleton.cpp
        src/gltf.cpp
        src/importer.cpp
        src/json.cpp
        src/live_import.cpp
        src/mesh.cpp
        src/model.cpp
        src/obj.cpp
        src/texture.cpp
        src/options.cpp
        src/pipeline.cpp
        src/primitive.cpp
        src/report.cpp
        src/sidecar.cpp
        src/unwrap.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    ${cy_import_animation_definitions}
    PUBLIC_DEPENDENCIES cy::core-assets cy::core-base cy::core-memory cy::core-values cy::core-math
                        cy::core-jobs
    # PRIVATE, and that is the isolation `thirdparty-dependencies` requires rather than a
    # convention: a consumer of cy::import inherits neither ufbx.h nor xatlas.h, so a translation
    # unit outside src/fbx.cpp, src/fbx_skeleton.cpp and src/unwrap.cpp cannot name one of their
    # types even by accident.
    # deps/manifest.toml records those two files as the interface, and this line is what makes the
    # record true.
    PRIVATE_DEPENDENCIES cy::dep::ufbx cy::dep::xatlas ${cy_import_animation_dependencies})

cy_add_module(
    NAME cy_import_cli
    LAYER tools
    TYPE EXECUTABLE
    SOURCES
        src/main.cpp
    PRIVATE_DEPENDENCIES cy::import cy::core-jobs)

# tests/ is read now and declares its suites later; see the comment at the top of it for why.
if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
