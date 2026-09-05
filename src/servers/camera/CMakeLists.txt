# src/servers/camera/ — layer 2: CyberCamera. Tasks 4.3.1, 4.3.2 and 4.3.3.
#
# `camera-system` reaches **Seed** at M4: the interfaces, the data model and the invariants exist,
# and dependents can be built against them. What is real and what is deliberately absent is written
# out in include/cy/servers/camera/server.h; README.md is the shorter version.
#
# WHY THIS DEPENDS ON cy::servers-render, WHICH IS THE ONE DEPENDENCY WORTH EXPLAINING. Both modules
# are at layer 2, so the layer checker permits the link (a module may depend on its own layer, never
# on a higher one). It exists because `cy::render::ViewDescription` is already exactly the shape
# `camera-system`'s "Render view production" requirement lists — M3 built it from that requirement
# and its header says so — and declaring a second view type here so the camera could own one would
# put two descriptions of a view in the tree for the second to drift from the first.
#
# THE DIRECTION IS ONE-WAY AND CHECKED BY THE BUILD. cy::servers-render names nothing from
# cy::camera; adding such a link would be caught in review, but it would also break the property
# that matters — a dedicated server build links the render model without a camera system, and a
# render test runs with no camera at all.
#
# WHAT DOES NOT BELONG HERE: an entity, a node, a world, a script (layer 2 forbids all four and the
# layer checker enforces it), a physics query (collision and occlusion are published as a batch and
# answered by the caller), a projection matrix (the renderer builds it from the semantic projection
# this module produces) and temporal jitter (the temporal framework's).
#
# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_servers_camera
    LAYER servers
    ALIAS cy::servers-camera
    TYPE STATIC
    SOURCES
        src/camera.cpp
        src/lens.cpp
        src/rig_compile.cpp
        src/rig_evaluate.cpp
        src/server.cpp
        src/smoothing.cpp
        src/stack.cpp
        src/view.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::core-values
                        cy::servers-render
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
