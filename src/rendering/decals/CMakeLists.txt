# src/rendering/decals/ — layer 4: decals in the device frame.
#
# `rendering-lighting-and-shadows` — "Decals". What a decal is — the projected box, its fades, its
# order and the budget that evicts it — is cy::rendering-lighting's `decals.h`, which has no device.
# Its assignment to clusters, as an element type of its own, is cy::rendering-assembly's. This module
# is the half with a device: the table a fragment reads decals from, its upload into the frame's
# bindless texture table, and the view-block words that name it. It is a module of its own for the
# reason src/rendering/light_probes/ is one: an edge between lighting and the pipeline would make the
# device-free half depend on a device.
#
# NO SHADER OF ITS OWN. The application is `cy/decal.slang`, called from `cy/frame.slang`'s forward
# fragment before its light loop; this module only fills what those functions read.
cy_add_module(
    NAME cy_rendering_decals
    LAYER rendering
    ALIAS cy::rendering-decals
    TYPE STATIC
    SOURCES src/decal_table.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-forward
                        cy::rendering-graph cy::rendering-lighting cy::rendering-pipeline cy::rhi)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
