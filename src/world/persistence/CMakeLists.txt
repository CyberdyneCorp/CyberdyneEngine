# src/world/persistence/ — the translation between the world's persistence overlay and a save's.
# M11.e, closing `m11a:save-has-an-engine-consumer`.
#
# A MODULE OF ITS OWN, AND THE REASON IS THE TWO BESIDE IT. cy::world owns the model the world
# maintains and says "no file I/O and no save format" (../CMakeLists.txt); cy::save owns the
# encoding and may not know an ECS component id (src/save/CMakeLists.txt). The translation needs
# both, so putting it in either would break that one's own rule. Here it is above both: LAYER scene
# like cy::world, linking cy::world, cy::ecs and cy::save — the first engine module above src/save/
# that does. See README.md.
cy_add_module(
    NAME cy_world_persistence
    LAYER scene
    ALIAS cy::world-persistence
    TYPE STATIC
    SOURCES
        src/save_translation.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::world cy::ecs cy::save cy::core-reflect cy::core-serialize)
