# src/rendering/forward/ — layer 4: the clustered forward frame. Tasks 4.3.1 through 4.3.4.
#
# `rendering-forward-clustered` at Working. Four subjects, and the division between them is the one
# the specification draws:
#
#   cluster.h      the grid, its exponential slicing constants, and the CPU reference assignment —
#                  the null backend's answer and the compute pass's check.
#   draw_list.h    the sort key's inputs, a radix sort that produces the same order as the render
#                  server's reference, and the per-draw instance record a draw's `first_instance`
#                  indexes.
#   frame.h        the thirteen stages, declared into the render graph with their reads and writes.
#                  A disabled feature is a pass that was never declared, which is what makes
#                  "their targets unallocated" observable rather than asserted.
#   diagnostics.h  what the frame did: cluster occupancy, per-pass draws and time, and whether the
#                  sort actually grouped the draws it promised to group.
#
# WHY THIS DEPENDS ON THE GRAPH AND THE OTHER THREE DO NOT. frame.h declares passes, so it names
# `cy::rendering-graph` — and through it the RHI's `Access` vocabulary, which is the only way a pass
# can say what it does. It still names no barrier, no layout and no stage: there is nowhere in
# `PassBuilder` for one, which is the M3 invariant seen from a pass author's side.
cy_add_module(
    NAME cy_rendering_forward
    LAYER rendering
    ALIAS cy::rendering-forward
    TYPE STATIC
    SOURCES
        src/cluster.cpp
        src/diagnostics.cpp
        src/draw_list.cpp
        src/frame.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-culling
                        cy::rendering-graph cy::rendering-lighting cy::servers-render)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
