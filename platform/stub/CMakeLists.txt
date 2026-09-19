# platform/stub/ — layer 3: the porting surface's own proof. M11.d task 4.4.
#
# It links NOTHING but cy::core-platform. That is the check: a port that needs no library, no
# window system and no operating-system facility beyond a clock and a place to write text still
# satisfies both interfaces, and every engine layer above still builds and runs against it.
#
# BUILT EVERYWHERE, ON EVERY HOST, IN EVERY PROFILE. A stub that were behind an option would be a
# stub nothing compiles, and the value of this module is entirely in its being compiled: it is the
# only implementation in the tree that answers false to every Feature, has no command line, no
# subprocesses, no dynamic loader, one core and one writable directory.
#
# `just build-engine --platform stub` builds this module's dependency closure and
# `integration.stub_platform`, which runs frames through Runtime::tick() on it — see just/build.just.

cy_add_module(
    NAME cy_platform_stub
    LAYER platform
    ALIAS cy::platform-stub
    SOURCES
        src/stub_platform.cpp
        src/stub_display_server.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-platform)
