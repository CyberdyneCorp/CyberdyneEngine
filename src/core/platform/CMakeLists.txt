# src/core/platform/ — layer 0: the engine-owned Platform and DisplayServer interfaces.
#
# Tasks 3.2.1, 3.3.1, 3.3.2, 3.3.5 and 3.3.6. The interfaces only, plus the machinery every
# implementation would otherwise write twice: the buffer convention, the feature-name table, the
# drop-and-warn flag filter, and the bounded event queue.
#
# There is no host #ifdef in this directory and no SDL header anywhere near it. Implementations live
# under platform/<name>/ (task 3.2.4), and tools/layercheck/ fails the build if either rule slips.

cy_add_module(
    NAME cy_core_platform
    LAYER core
    ALIAS cy::core-platform
    SOURCES
        src/display_server.cpp
        src/display_support.cpp
        src/platform.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base)
