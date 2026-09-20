# samples/00-empty — the M0 milestone artefact. Task 3.6.1.
#
# LAYER tools: a sample consumes the whole engine rather than sitting in the stack, which is the
# position tools/layercheck/layercheck.py gives samples/ for the include check as well.
#
# It links both display backends because it chooses between them at run time — `--headless` is what
# CI and tests/smoke/ use, and the same binary opens a real window without it.

#
# M11.d task 4.3 adds `--platform`, and with it the two backends this milestone wrote: the STUB,
# which exists on every host, and the NATIVE Linux one, which exists only where it can be built. The
# native backend is therefore a conditional dependency and a compile definition rather than an
# unconditional link — the one place in this sample where the build's shape reaches the source, and
# it is here rather than in the engine because a SAMPLE choosing among the backends its build
# actually has is a consumer's decision. No engine layer gained an #ifdef for this.
set(cy_sample_empty_definitions CY_SAMPLE_LINUX_NATIVE=0)
set(cy_sample_empty_platforms cy::platform-headless cy::platform-desktop-sdl3 cy::platform-stub)
if(TARGET cy_platform_linux_native)
    list(APPEND cy_sample_empty_platforms cy::platform-linux-native)
    set(cy_sample_empty_definitions CY_SAMPLE_LINUX_NATIVE=1)
endif()

cy_add_module(
    NAME cy_sample_empty
    LAYER tools
    TYPE EXECUTABLE
    ALIAS cy::sample-empty
    SOURCES main.cpp
    PRIVATE_DEFINITIONS ${cy_sample_empty_definitions}
    PRIVATE_DEPENDENCIES
        cy::runtime
        cy::platform-host
        ${cy_sample_empty_platforms})
