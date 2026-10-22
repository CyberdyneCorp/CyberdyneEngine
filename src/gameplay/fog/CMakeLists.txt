# src/gameplay/fog/ — layer 4: fog of war, the simulation half. `gameplay-framework` — "Fog of war".
#
# Which cells each team sees and has seen, and the ghosts of the structures it remembers, updated in
# the fixed step from the units' authoritative positions. A SEPARATE TARGET for the reason
# abilities/ is one: a game without fog of war links none of it. It depends on cy::gameplay for
# `TeamId` and on nothing that draws: what a renderer makes of the grid is
# src/rendering/fog_of_war/, which READS `FogOfWar::write_states` and writes nothing back.
#
# INTEGER ARITHMETIC ONLY. Positions arrive as Q32.32 metres (`detmath::Fixed::raw`) and are located
# on the grid by integer division, so the module declares the CrossPlatform profile without linking
# cy::core-detmath, and `unit.gameplay_fog` holds a committed digest that every CI leg reproduces.
cy_add_module(
    NAME cy_gameplay_fog
    LAYER scene
    ALIAS cy::gameplay-fog
    TYPE STATIC
    SOURCES src/fog_of_war.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::gameplay)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cy_gameplay_fog PRIVATE -ffp-contract=off)
elseif(MSVC)
    target_compile_options(cy_gameplay_fog PRIVATE /fp:precise)
endif()

cy_declare_determinism_profile(cy_gameplay_fog
    PROFILE CrossPlatform
    REASON "every quantity is an integer: Q32.32 positions are located on the grid by integer division and a cell is in sight by an integer squared-distance comparison")

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
