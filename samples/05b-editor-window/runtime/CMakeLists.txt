# samples/05b-editor-window/runtime/ — the ENGINE on the far end of the editor's viewport.
# M7 task 5b.1.
#
# --- WHY THE M5.5 DIRECTORY GREW A C++ TARGET -----------------------------------------------------
#
# Its own CMakeLists.txt said, at M5.5: "A C++ driver would link no engine code and prove nothing
# about the engine." That was true of the DRIVER and it is still true — `window.py` is still the
# artefact. What was missing was the other process: the thing the driver points the editor at.
#
# Until M7 that was `cy-viewport-publisher`, a fixture in the editor's Cargo workspace that clears
# an image to a colour and moves a white bar. This target replaces it with the engine: M3's scene
# and renderer, through the render graph, on a real device, published over the transport the editor
# already speaks, with the gizmo geometry `cy::render::build_gizmo_layout` produced.
#
# --- WHAT IT LINKS, AND WHY EACH ------------------------------------------------------------------
#
#   cy::sample-first-light      the scene and the renderer. Reused rather than reimplemented: a
#                               second renderer would drift from the one `render.golden` photographs.
#   cy::backends-viewport       the dma-buf ring, the timeline semaphores and the socket.
#   cy::servers-render          the gizmo geometry, `project_to_pixel`, and M6's control half of
#                               the viewport transport — which until this artefact was reachable
#                               from its own tests and from nothing else.
#   cy::runtime-editor-bridge   the editor's control socket.
#   cy::rhi-vulkan              the device. Named rather than defaulted — see main.cpp for why a
#                               fall-back to the null backend would be worse than a refusal.
#
# --- WHERE IT IS NOT DECLARED ---------------------------------------------------------------------
#
# Everything above needs a Vulkan device that can export a dma-buf, which is Linux with
# CY_RENDERER_VULKAN. The parent directory has already returned on a machine with no display; this
# adds the two conditions that are about the device rather than about the window.

if(NOT CY_RENDERER_VULKAN OR NOT LINUX)
    message(STATUS "samples/05b-editor-window/runtime: the engine's viewport publisher needs a "
                   "Vulkan device that can export a dma-buf; the target is not declared")
    return()
endif()

cy_add_module(
    NAME cy_editor_window_runtime
    LAYER tools
    TYPE EXECUTABLE
    SOURCES
        main.cpp
        overlay.cpp
        session.cpp
    PRIVATE_DEPENDENCIES
        cy::sample-first-light
        cy::backends-viewport
        cy::servers-render
        cy::runtime-editor-bridge
        cy::rhi
        cy::rhi-null
        cy::rhi-vulkan)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
