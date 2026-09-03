# samples/00-empty — the M0 milestone artefact. Task 3.6.1.
#
# LAYER tools: a sample consumes the whole engine rather than sitting in the stack, which is the
# position tools/layercheck/layercheck.py gives samples/ for the include check as well.
#
# It links both display backends because it chooses between them at run time — `--headless` is what
# CI and tests/smoke/ use, and the same binary opens a real window without it.

cy_add_module(
    NAME cy_sample_empty
    LAYER tools
    TYPE EXECUTABLE
    ALIAS cy::sample-empty
    SOURCES main.cpp
    PRIVATE_DEPENDENCIES
        cy::runtime
        cy::platform-host
        cy::platform-headless
        cy::platform-desktop-sdl3)
