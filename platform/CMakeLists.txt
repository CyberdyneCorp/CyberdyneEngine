# platform/ — layer 3: the porting surface.
#
# Each backend is a target declared through cy_add_module(). Which ones are built is a property of
# the host and of the feature options, not of this file's ordering.
#
# headless/ first, because it is the one every test that is not about a window runs against and the
# one CI depends on; desktop-sdl3/ second. Neither depends on the other — both implement the
# interfaces in src/core/platform/ and nothing else.
#
# `core-platform-abstraction`, "New platform port": a new platform implements Platform,
# DisplayServer, an input backend, an audio backend and a graphics surface provider, "with no
# changes required in src/core/ or above". It is a directory here and one line below.
add_subdirectory(headless)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_subdirectory(ios)
else()
    add_subdirectory(desktop-sdl3)
endif()

# stub/ — M11.d task 4.4. The porting surface's own proof: a Platform and a DisplayServer that
# share no desktop assumption at all. Built on every host, because a stub nothing compiles proves
# nothing.
add_subdirectory(stub)

# linux-native/ — M11.d task 4.1. The SECOND native implementation of both interfaces, which is
# what proves the abstraction carries no SDL assumption. desktop-sdl3/ is NOT replaced: two
# implementations is the proof, and one of them being deleted would leave the interface proved by
# exactly as many as before.
#
# Guarded by the host rather than by a feature option: a native backend for another desktop is a
# directory beside this one and one branch here, which is the same shape desktop-sdl3/ uses for its
# per-operating-system host files.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(linux-native)
endif()

# host/ — the desktop host's frame loop (task 3.4.3). It implements neither Platform nor
# DisplayServer: it is the `while (running)` that calls Runtime::tick(), living here because
# `core-platform-abstraction` requires the loop to be the platform's and not the engine's.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_subdirectory(host)
endif()
