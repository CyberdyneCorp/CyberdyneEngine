# src/runtime/editor_bridge/ — layer 5: the hosted runtime's side of the editor's control socket.
# M7 tasks 5b.3 and 5b.4.
#
# --- A TARGET OF ITS OWN, BESIDE cy::runtime RATHER THAN INSIDE IT --------------------------------
#
# Because a headless runtime, a dedicated server and every sample that is not an editor session have
# no use for a Unix socket speaking the editor's protocol, and `cy::runtime` is what all of them
# link. Keeping it separate is what makes "this build talks to an editor" visible in a link line.
#
# --- WHY IT NAMES NO SERVER ----------------------------------------------------------------------
#
# It decodes and encodes; what a pick or a gizmo intent MEANS is the host's, because the host is
# what holds a scene. So this target depends on core alone, and `samples/05b-editor-window/runtime/`
# is what joins it to `cy::render::build_gizmo_layout` and to a world. A bridge that owned the
# answers would be a bridge that owned the world.
#
# Linux, like the socket it is built on. Every host that links it is Linux-only for the same reason.
if(NOT UNIX OR APPLE)
    message(STATUS "src/runtime/editor_bridge: the editor bridge is a Unix domain socket; the "
                   "module is not declared on this platform")
    return()
endif()

cy_add_module(
    NAME cy_runtime_editor_bridge
    LAYER runtime
    ALIAS cy::runtime-editor-bridge
    TYPE STATIC
    SOURCES src/bridge.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
