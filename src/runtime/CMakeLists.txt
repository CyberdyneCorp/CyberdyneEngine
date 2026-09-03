# src/runtime/ — layer 5: bootstrap, subsystem wiring, and Runtime::tick().
#
# Tasks 3.4.1 and 3.4.2. The runtime never owns a loop: the `while (running)` is the host's, under
# platform/, and this target links no host and no backend. It consumes the interfaces in
# src/core/platform/ and the trace in src/core/diagnostics/, and nothing else exists yet for it to
# wire.
#
# cy::core-platform is PUBLIC because Platform& and DisplayServer& are in the runtime's own
# interface; cy::core-diagnostics is PRIVATE because the trace is something the runtime writes to,
# not something a consumer of the runtime has to know about.

cy_add_module(
    NAME cy_runtime
    LAYER runtime
    ALIAS cy::runtime
    SOURCES src/runtime.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-platform
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

# The startup-order probe. It starts and stops a runtime, prints the journal, and exits — which is
# the only way to compare the order across *processes* rather than across iterations of one loop.
# tests/smoke/test_startup_order.cpp runs it a hundred times (task 3.4.4).
#
# Behind CY_BUILD_TESTS: it exists for that test and a shipping tree has no use for it.
if(CY_BUILD_TESTS)
    cy_add_module(
        NAME cy_runtime_startup_probe
        LAYER runtime
        TYPE EXECUTABLE
        SOURCES probe/startup_order_probe.cpp
        PRIVATE_DEPENDENCIES cy::runtime cy::platform-headless cy::platform-desktop-sdl3)
endif()
