# src/sequencing/ — CyberSequence, M8.c section 3.
#
# THREE TARGETS, and the split is the specification's first requirement rather than a preference.
#
#   cy::sequencing            the whole of the timeline: exact time, the authored model, the
#                             compiler and its two indexes, the compiled program, evaluation into
#                             per-subsystem batches, arbitration, capture and restore, seeking,
#                             skipping and the preload plan. It names NO subsystem.
#   cy::sequencing-camera     the bridge that turns a camera batch into camera stack calls.
#   cy::sequencing-gameplay   the bridge that turns a command batch into gameplay commands.
#
# `sequencing-and-cinematics` opens with "Sequences orchestrate, they do not own" — a sequence
# "SHALL NOT contain its own camera evaluation, animation playback, audio playback, effect
# simulation, environment state, or gameplay state", and "There SHALL NOT be a cinematic-only
# implementation of any subsystem the engine already provides."
#
# A link graph is how that becomes structural rather than reviewed. `cy::sequencing` depends on
# `cy::core` and nothing else: it CANNOT contain a camera evaluation, because the camera server's
# header is not on its include path, and the same is true of every other subsystem. What it produces
# is values in a batch. The two bridges are what may name a subsystem, they are one file each, and
# nothing depends on them — so a project that plays sequences without a camera links neither.
#
# LAYER. `scene` (4), the rung src/gameplay/, src/world/, src/graph/ and src/camera/ all chose, for
# the reason each of their CMakeLists gives: `engine-architecture`'s table has no rung for these and
# inventing one is not this milestone's change to make. The core target would sit lower — it depends
# on layer 0 alone — but the two bridges need layer 2 and layer 4, and three targets in one
# directory at one layer is simpler to reason about than a directory that spans two.
#
# NOTHING HERE IS BEHIND A CY_* OPTION, for the reason src/camera/ gives: a subsystem that exists in
# some configurations has a suite that runs in some configurations. `sequencing-and-cinematics`
# declares no removal option, unlike `animation-and-skinning`'s CY_ANIMATION and `ai-system`'s CY_AI.
cy_add_module(
    NAME cy_sequencing
    LAYER scene
    ALIAS cy::sequencing
    TYPE STATIC
    SOURCES
        src/adapters.cpp
        src/compile.cpp
        src/dispatch.cpp
        src/player.cpp
        src/program.cpp
        src/source.cpp
        src/time.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::core-values)

add_subdirectory(camera)
add_subdirectory(gameplay)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

# artefact/ — the capture of a cut mid-blend. Declared last because it links both bridges, and it is
# a program rather than a library: see artefact/CMakeLists.txt.
add_subdirectory(artefact)
