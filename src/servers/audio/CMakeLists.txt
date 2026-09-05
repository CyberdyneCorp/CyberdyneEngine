# src/servers/audio/ — layer 2: the engine-owned `AudioServer`. Tasks 4.3.4 and 4.3.5.
#
# `audio` reaches **Seed** at M4: the driver interface, the bus graph, voices, playback and basic
# spatialisation. What is real and what is deliberately absent is written out in
# include/cy/servers/audio/server.h; README.md is the shorter version.
#
# WHY THIS TARGET IS NOT BEHIND CY_AUDIO, AND THE MINIAUDIO BACKEND IS. `audio` puts the policy in
# the engine — "`AudioServer`, the bus graph, voice management, streaming policy, spatialisation
# policy, and the importance system SHALL be engine code" — and puts the library behind an interface.
# So the interface, the null backend and every suite over them are ALWAYS built, for the reason
# src/abi/ is not behind CY_SCRIPTING: an interface that exists in some configurations has a test
# suite that runs in some configurations, which is M3's CY_RENDERER_VULKAN mistake with a longer
# tail. CY_AUDIO gates src/backends/audio-miniaudio/ and the miniaudio fetch, which is what
# `audio`'s "miniaudio SHALL be a required dependency whenever `CY_AUDIO` is enabled, and SHALL be
# excluded entirely when it is not" actually asks for.
#
# WHAT DOES NOT BELONG HERE: a miniaudio type (layer 3's, and no header outside
# src/backends/audio-miniaudio/ may name one), an entity or a node (layer 2 forbids both), an
# acoustics backend (M8), and any allocation on a path the realtime callback can reach.
#
# Sources are listed explicitly and never globbed.
cy_add_module(
    NAME cy_servers_audio
    LAYER servers
    ALIAS cy::servers-audio
    TYPE STATIC
    SOURCES
        src/backend.cpp
        src/bus.cpp
        src/format.cpp
        src/server.cpp
        src/spatial.cpp
        src/voice.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::core-values
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
