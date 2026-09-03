# src/core/values/ — layer 0: the vocabulary types the scripting, serialization, editor and ABI
# boundaries all speak.
#
# Section 1.3. Governed by `core-type-system`. `Var`, generational handles, asset ids as a type
# distinct from handles, events and signals, `Callable`, and string interning.
#
# Separate from reflect/ because the split is load-bearing: a handle or an interned string is used
# on hot paths, and reflection is not. Putting them in one module would blur that line.
#
# THE DEPENDENCY ON diagnostics IS PRIVATE, AND IT INVERTS THE ORDER IN src/core/CMakeLists.txt.
# Task 1.3.6 puts the value layer's counters on the M0 trace, so src/diagnostics.cpp includes
# <cy/core/diagnostics/...>. No public header of this module does, so nothing that includes `Var`
# acquires the trace's headers. `add_subdirectory(values)` currently runs before
# `add_subdirectory(diagnostics)`; CMake resolves `cy::core-diagnostics` at generate time so the
# link is correct either way, but the comment in that file describes the list as a dependency order
# and the values line therefore belongs after diagnostics. That file is not this module's to edit.

# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_core_values
    LAYER core
    ALIAS cy::core-values
    TYPE STATIC
    SOURCES
        src/asset_id.cpp
        src/callable.cpp
        src/counters.cpp
        src/diagnostics.cpp
        src/handle.cpp
        src/name.cpp
        src/signal.cpp
        src/var.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PUBLIC_DEPENDENCIES cy::core-base
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

# tests/ is read now and declares its suites later; see the comment at the top of it for why.
if(CY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
