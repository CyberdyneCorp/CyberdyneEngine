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
add_subdirectory(desktop-sdl3)

# host/ — the desktop host's frame loop (task 3.4.3). It implements neither Platform nor
# DisplayServer: it is the `while (running)` that calls Runtime::tick(), living here because
# `core-platform-abstraction` requires the loop to be the platform's and not the engine's.
add_subdirectory(host)
