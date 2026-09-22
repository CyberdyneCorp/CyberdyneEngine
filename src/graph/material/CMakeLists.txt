# src/graph/material/ — the material lowering. M11.c task 6.1a.
#
# ================================================================================================
# WHY THIS IS A TARGET OF ITS OWN AND NOT A FIFTH `lower_*.cpp` IN cy_graph
# ================================================================================================
#
# `src/graph/CMakeLists.txt` states the invariant a fifth file would have broken, in its own words:
#
#   "Nothing here depends on anything above core: a graph compiler that needed a scene, a device or
#    a server would be a compiler that cannot run in a cook."
#
# `cy::rendering-material` depends on `cy::servers-render`, so a `lower_material.cpp` inside
# `cy_graph` would have put the render server into the link closure of every consumer of the
# authoring layer — `visual-scripting`, `ai-system`, `animation-and-skinning` and the editor's own
# transport. The four existing lowerings each compile to an IR their own module owns and none of them
# reaches outside core; the material IR is the material compiler's, and it lives at layer 4 under
# `rendering`.
#
# So the dependency points the one way the layering allows: this target knows about `cy::graph` and
# about `cy::rendering-material`, and neither of them knows about this.
#
# IT IS STILL UNDER src/graph/ ON PURPOSE. M11.c's task list names the absence as "a `lower_material`
# in `src/graph/`, which today has `lower_script`, `lower_behaviour`, `lower_camera` and `lower_pose`
# and no material lowering at all", and a reader looking for the engine's lowerings should find all
# five in one place.

cy_add_module(
    NAME cy_graph_material
    LAYER rendering
    ALIAS cy::graph-material
    TYPE STATIC
    SOURCES
        src/canvas.cpp
        src/lower_material.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::graph cy::rendering-material)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
