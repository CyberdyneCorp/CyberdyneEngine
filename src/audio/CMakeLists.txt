# src/audio/ — CyberAudio's acoustics half: the acoustics backend and its geometry, the asynchronous
# simulation, importance tiers and virtualisation, the effect chain, and interactive music.
# M8.b tasks 10.1 and 10.2.
#
# WHAT IS HERE AND WHY IT IS NOT src/servers/audio/. `audio` reached its M4 tier in
# src/servers/audio/ at layer 2: the `AudioServer`, the bus graph, voices, spatialisation, the
# command queue and the null backend, with miniaudio behind `CY_AUDIO` in src/backends/. What that
# module deliberately left is everything this one is — acoustics, tiers, effects and interactive
# music — and this module sits ABOVE it because two of those need what layer 2 may not name:
#
#   acoustic geometry   comes from the world's collision geometry, through an extraction interface
#                       the host implements. `audio`: "Audio SHALL consume geometry through an
#                       extraction interface and SHALL NOT call into the physics server directly."
#   importance scoring  reads a listener and a world position per source per frame.
#
# STEAM AUDIO IS DECLARED AND GATED. deps/manifest.toml carries it with `optional = true` and
# `feature = "CY_AUDIO_STEAM_AUDIO"`, which defaults OFF, so a default build fetches nothing and
# contains none of its code. src/acoustics.cpp is the ONE translation unit that may name a Steam
# Audio type, and it does so inside `#if defined(CY_AUDIO_STEAM_AUDIO)`; `FallbackAcoustics` is what
# every build has, which is the requirement's "content SHALL NOT depend on Steam Audio being
# present".
#
# LAYER. `scene` (4), beside src/graph/, src/world/ and src/ui/.
cy_add_module(
    NAME cy_audio
    LAYER scene
    ALIAS cy::audio-acoustics
    TYPE STATIC
    SOURCES
        src/acoustics.cpp
        src/effects.cpp
        src/interactive.cpp
        src/tiers.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::core-values)

# Issue #87: the stream player — tracks, refill jobs, crossfades, seeks — over the server's streams.
#
# A MODULE OF ITS OWN, AND THE REASON IS A DEFECT THIS ONE DOES NOT FIX. cy_audio above declares
# `cy::audio::SimulationTier`, `TierBudgets` and `EffectKind` (tiers.h, effects.h) and the server
# declares different types of the same names in the same namespace (voice.h, server.h, bus.h). No
# binary linked both until the stream player — which needs the server — joined this directory, and
# the editor's suites then failed to link on two definitions of `effect_kind_name(EffectKind)`.
# Keeping the player out of cy_audio keeps the two apart, as they have always been; reconciling the
# acoustics half's copies with the server's is its own change (see the module README).
cy_add_module(
    NAME cy_audio_streaming
    LAYER scene
    ALIAS cy::audio-streaming
    TYPE STATIC
    SOURCES
        src/streaming.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-assets cy::core-base cy::core-memory cy::core-values
                        cy::servers-audio
    PRIVATE_DEPENDENCIES cy::core-jobs cy::core-math)

# The Steam Audio dependency, when the option is on. The wrapper target cmake/dependencies.cmake
# publishes is `cy::dep::steam_audio`; nothing above src/acoustics.cpp links it, and no header here
# names one of its types.
if(CY_AUDIO_STEAM_AUDIO AND TARGET cy::dep::steam_audio)
    target_link_libraries(cy_audio PRIVATE cy::dep::steam_audio)
endif()

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
