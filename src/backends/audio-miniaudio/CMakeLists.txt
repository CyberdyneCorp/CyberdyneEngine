# src/backends/audio-miniaudio/ — layer 3: the default `AudioBackend`, over miniaudio. Task 4.3.4.
#
# THE ONLY DIRECTORY IN THE ENGINE THAT MAY NAME AN `ma_` SYMBOL, and the only one that includes
# miniaudio.h. `audio`: "No backend type SHALL appear in any engine or game-facing header outside its
# own backend module", with the scenario "**WHEN** engine or game code is compiled **THEN** no
# backend library type SHALL appear in any header outside `backends/audio/`."
#
# HOW THAT IS KEPT, AND WHERE IT IS NOT YET CHECKED. The public header declares an opaque
# `MiniaudioState*` and includes nothing from miniaudio, so an upward include is impossible by
# construction; the layer checker fails the build on an include that reaches above layer 3. What it
# does NOT yet have is the equivalent of tools/layercheck/layercheck.py's `sdl` and `gpuapi` checks —
# a rule that names miniaudio.h and refuses it outside this directory. That file is not this module's
# to edit; the rule belongs there and is recorded for whoever owns it.
#
# WHAT THIS TARGET IS ASKED OF MINIAUDIO, in full, because `audio` requires the surface to be
# documented: device enumeration (`ma_context`), the output device and its realtime callback
# (`ma_device`), and the format conversion that follows from asking for `ma_format_f32` at the
# engine's rate. NOT its node graph, NOT its high-level engine API, NOT its 3D spatialisation — the
# specification names all three as things the engine owns instead. The check is one command:
#     grep -o 'ma_[a-z_]*' src/backends/audio-miniaudio/src/*.cpp | sort -u
cy_add_module(
    NAME cy_backends_audio_miniaudio
    LAYER backends
    ALIAS cy::backends-audio-miniaudio
    TYPE STATIC
    SOURCES
        src/miniaudio_backend.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::servers-audio
    # cy::dep::miniaudio is PRIVATE: it brings miniaudio.h's include directory with it, and a public
    # dependency would put that directory on the include path of everything that links this module —
    # which is precisely the leak the requirement above forbids.
    PRIVATE_DEPENDENCIES cy::core-diagnostics cy::dep::miniaudio)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
