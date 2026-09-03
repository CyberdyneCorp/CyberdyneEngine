# src/core/base/ — layer 0: type aliases, cy::Expected<T, Error>, the Error model, the assertion
# macros, and the diagnostic seam every layer above reports a warning through.
#
# Tasks 3.1.1, 3.1.2 and 3.1.3. Nothing else belongs here: containers, allocators, math and the job
# system are M1, written against `core-type-system` once it exists (design.md §9).

cy_add_module(
    NAME cy_core_base
    LAYER core
    ALIAS cy::core-base
    SOURCES
        src/assert.cpp
        src/diagnostic_sink.cpp
        src/error.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include")
