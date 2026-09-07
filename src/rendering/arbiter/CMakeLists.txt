# src/rendering/arbiter/ — layer 4: the renderer's one budget arbiter, its subsystem controllers,
# the named renderer profiles and the rendering configuration asset. M7 tasks 10.1 and 10.2.
#
# `rendering-architecture` reaches Complete here, and this module is why: six paged or stochastic
# systems arrived in this milestone and each of them can spend an unbounded amount of a frame. The
# arbiter is what makes them one budget rather than six competing ones.
#
# NO DEVICE, NO GRAPH AND NO SHADER, and no dependency on any of the six subsystems it arbitrates.
# That is not tidiness; it is the criterion. A subsystem declares a priced ladder and reports its
# own cost, and the arbiter answers with milliseconds — so the whole control law is exercisable at
# 71 load magnitudes in a second of CPU, which is the only way a law over a discrete ladder can be
# certified at all (`design.md` §2.1: one step magnitude can make any law look stable). An arbiter
# that named `cy::rendering-shadows` would be a control law you could only test by rendering.
#
# It does not depend on cy::servers-residency either, and the two are deliberately not merged:
# `residency`'s levers are arbitrated in BYTES by the memory pressure monitor and these in
# MILLISECONDS by the renderer. `subsystem.h` says where the two meet.
cy_add_module(
    NAME cy_rendering_arbiter
    LAYER rendering
    ALIAS cy::rendering-arbiter
    TYPE STATIC
    SOURCES
        src/arbiter.cpp
        src/configuration.cpp
        src/profiles.cpp
        src/subsystem.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
