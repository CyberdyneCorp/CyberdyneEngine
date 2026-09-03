# src/core/memory/ — layer 0: the allocator interface everything above allocates through, the memory
# domains and the budget tree that account for it, and the containers written against it.
#
# Section 2. Governed by `core-memory-and-containers`. Allocator interface and propagation, domains
# and budgets, pressure levels, sequence and associative containers, handle pools, chunked storage,
# scratch and frame memory, retirement and frame epochs, virtual address reservation.
#
# Chunked storage lands here rather than in the ECS (design.md §7): it is memory machinery with no
# knowledge of components, archetypes or queries, and M2's ECS is its first consumer.
#
# TWO DEPENDENCIES THAT INVERT THE ORDER IN src/core/CMakeLists.txt, and are correct anyway.
#
#   cy::core-values is PUBLIC. Task 2.5's handle pools compose `cy::GenerationTable` and
#   `cy::Handle<Tag>` rather than reimplementing them — <cy/core/values/handle.h> says so from the
#   other side, naming this task. A second generation scheme would give the engine two answers to
#   "is this handle stale".
#
#   cy::core-diagnostics is PRIVATE. Task 2.11 puts the memory counters on the M0 trace, so
#   src/diagnostics.cpp and src/pressure.cpp include <cy/core/diagnostics/...>. No public header of
#   this module does, so nothing that includes `Array` acquires the trace's headers.
#
# Both links resolve because CMake resolves cy::* names at generate time. But that file's comment
# calls its list a dependency order, so `add_subdirectory(memory)` belongs after values and
# diagnostics. It is not this module's file to edit; see src/core/memory/README.md.

# Reserve-and-commit has no portable spelling, so it is two translation units selected here rather
# than one file with an #ifdef in it — the same shape src/core/diagnostics/ uses for its crash
# handlers. The Windows unit has never been compiled; see the comment at the top of it.
if(WIN32)
    set(cy_memory_platform_sources src/virtual_memory_windows.cpp)
else()
    set(cy_memory_platform_sources src/virtual_memory_posix.cpp)
endif()

# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_core_memory
    LAYER core
    ALIAS cy::core-memory
    TYPE STATIC
    SOURCES
        src/allocator.cpp
        src/arena.cpp
        src/budget.cpp
        src/chunk_allocator.cpp
        src/chunk_storage.cpp
        src/diagnostics.cpp
        src/domain.cpp
        src/epoch.cpp
        src/frame_memory.cpp
        src/hash.cpp
        src/lifetime.cpp
        src/pressure.cpp
        src/scope.cpp
        src/slab.cpp
        src/system_allocator.cpp
        src/tracking_allocator.cpp
        src/virtual_arena.cpp
        ${cy_memory_platform_sources}
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-values
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

# tests/ is read now and declares its suites later; see the comment at the top of it for why.
if(CY_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# The general-heap measurement of task 2.10. Not a test: it is a measurement whose numbers are in
# README.md, and `just` runs it on demand.
if(CY_BUILD_TESTS)
    cy_add_module(
        NAME cy_memory_heap_pattern
        LAYER tools
        TYPE EXECUTABLE
        SOURCES bench/heap_pattern.cpp
        PRIVATE_DEPENDENCIES cy::core-memory)
endif()
