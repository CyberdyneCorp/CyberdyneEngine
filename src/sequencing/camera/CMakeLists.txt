# src/sequencing/camera/ — the bridge from a camera batch to the camera stack. M8.c task 3.2.
#
# ONE FILE, and it is the only place in this milestone that may name a camera. `cy::sequencing`
# depends on `cy::core` alone, so a camera evaluation cannot be written inside it — the header is
# not on its include path — and this target is where the batch it produced is turned into
# `CameraStack::push()`, `CameraStack::release()` and `CameraServer::cut()`.
#
# `cy::servers-camera` is layer 2 and this target declares layer 4 beside its own module, so the
# link is downward and the layer checker is what keeps it that way.
cy_add_module(
    NAME cy_sequencing-camera
    LAYER scene
    ALIAS cy::sequencing-camera
    TYPE STATIC
    SOURCES
        src/bridge.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::sequencing cy::servers-camera)
