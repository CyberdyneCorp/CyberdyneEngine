# src/animation/assets/ — cooked skeletons, clips and pose programs: their records, and the library
# that loads them through the asset system and binds rigs by asset id. Issue #76 stage 2.
#
# THE RUNTIME HALF OF WHAT tools/import/ COOKS. The importer writes the skeleton and clip records;
# until this module the only reader was in that cook-time library, so a game had to link the
# importer to load a character. `cooked.h` reads the same bytes with nothing of `cy::import`, and
# adds the record a program needs to be loaded rather than compiled at startup.
#
# NO COMPILER. It depends on `cy::graph` for the program TYPES and `assemble_pose_program`, which
# live in pose_program.cpp; `compile_pose` lives in lower_pose.cpp and nothing here names it, so a
# binary that loads programs does not link the compiler. tests/runtime_only_probe.cpp makes that a
# link-time fact rather than a claim.
#
# LAYER. `scene` (4), beside the runtime it decodes into; `cy::core-assets` is layer 0.
cy_add_module(
    NAME cy_animation_assets
    LAYER scene
    ALIAS cy::animation-assets
    TYPE STATIC
    SOURCES
        src/cooked.cpp
        src/library.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::animation cy::core-assets cy::core-base cy::core-memory
                        cy::core-values cy::graph)
