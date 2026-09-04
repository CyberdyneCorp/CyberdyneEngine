# samples/01-headless-host — the M1 milestone artefact. Tasks 5.1 and 5.2.
#
# LAYER tools, like every sample: it consumes the whole engine and nothing consumes it.
#
# It links the five core modules M1 built and nothing else. There is no platform, no display server
# and no runtime here on purpose — a headless host is what the substrate can do on its own, and a
# sample that needed a window to prove it would be proving something else.
#
# The source list is explicit, as everywhere in this tree: a file the list does not name is compiled
# by nothing and reads in review exactly like a file that passes.

cy_add_module(
    NAME cy_sample_headless-host
    LAYER tools
    TYPE EXECUTABLE
    ALIAS cy::sample-headless-host
    SOURCES
        main.cpp
        content.cpp
        simulation.cpp
    PRIVATE_DEPENDENCIES
        cy::core-assets
        cy::core-config
        cy::core-jobs
        cy::core-memory
        cy::core-reflect)
