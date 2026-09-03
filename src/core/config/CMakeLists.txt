# src/core/config/ — layer 0: the project manifest, the project graph built from it, and layered
# typed configuration.
#
# Section 4. Governed by `project-and-plugins`. M0 seeded that capability on its module half only;
# the manifest here is what "the project graph is authoritative" actually refers to. Layering
# enforcement extends from targets to the project graph, and an undeclared dependency or a cycle is
# an error rather than a warning.

# One placeholder source, replaced by the first real one. Sources are listed explicitly and never
# globbed: a file this list does not name is compiled by nothing, which reads in a review exactly
# like a file that passes.
cy_add_module(
    NAME cy_core_config
    LAYER core
    ALIAS cy::core-config
    TYPE STATIC
    SOURCES
        src/placeholder.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base)
