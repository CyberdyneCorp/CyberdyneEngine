# samples/02-headless-sim — the M2 milestone artefact. Tasks 5.1, 5.2 and 5.3.
#
# LAYER tools, like every sample: it consumes the whole engine and nothing consumes it.
#
# Four dependencies, one per workstream this milestone built, and the transitive closure is the
# argument for the sample existing at all:
#
#   cy::scene-serialization  brings cy::scene, and through it cy::ecs and cy::core-serialize — the
#                            authoring documents, the cook, and the world the cook fills.
#   cy::runtime              brings the fixed-tick loop, the simulation and the commit boundary,
#                            and through them cy::core-determinism.
#   the two platform modules  a runtime needs a Platform and a DisplayServer. Headless, because a
#                            simulation that needed a window to be reproducible would be measuring
#                            the window.
#
# The source list is explicit, as everywhere in this tree: a file the list does not name is compiled
# by nothing and reads in review exactly like a file that passes.

cy_add_module(
    NAME cy_sample_headless-sim
    LAYER tools
    TYPE EXECUTABLE
    ALIAS cy::sample-headless-sim
    SOURCES
        main.cpp
        content.cpp
        simulation.cpp
    PRIVATE_DEPENDENCIES
        cy::scene-serialization
        cy::runtime
        cy::platform-headless
        cy::platform-desktop-sdl3)
