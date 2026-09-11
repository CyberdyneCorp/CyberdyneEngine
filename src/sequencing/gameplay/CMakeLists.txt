# src/sequencing/gameplay/ — the bridge from a command batch to the gameplay command stream.
# M8.c task 3.4.
#
# A SEQUENCE IS THE SIXTH COMMAND PRODUCER, alongside human input, artificial intelligence, network
# peers, replay and automation — and it opens a producer buffer exactly like the other five. That is
# the whole of this target: `cy::sequencing` names no gameplay type, so the conversion happens here,
# and the conversion is deliberately unable to do anything the other producers cannot.
cy_add_module(
    NAME cy_sequencing-gameplay
    LAYER scene
    ALIAS cy::sequencing-gameplay
    TYPE STATIC
    SOURCES
        src/commands.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::gameplay cy::sequencing)
