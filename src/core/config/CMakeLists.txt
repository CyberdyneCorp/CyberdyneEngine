# src/core/config/ — layer 0: the project manifest, the project graph built from it, and layered
# typed configuration.
#
# Section 4. Governed by `project-and-plugins`. M0 seeded that capability on its module half only;
# the manifest here is what "the project graph is authoritative" actually refers to. Layering
# enforcement extends from targets to the project graph, and an undeclared dependency or a cycle is
# an error rather than a warning.
#
# include(project) is what does that enforcement, and it does it at configure time: it validates the
# project manifest through tools/project/, renders <cy_project.h> from what survives, and arms the
# transitive layer check over the whole target graph. It is included from here rather than from the
# top-level CMakeLists.txt because cy::core-config is what consumes its output; the top level beside
# include(modules) is the natural home and should take it over when that file is next touched.
include(project)

# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_core_config
    LAYER core
    ALIAS cy::core-config
    TYPE STATIC
    SOURCES
        src/module_registry.cpp
        src/project.cpp
        src/settings.cpp
    PUBLIC_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CY_PROJECT_GENERATED_INCLUDE_DIR}"
    PUBLIC_DEPENDENCIES cy::core-base)

# tests/ is read now and declares its suites later; see the comment at the top of it for why.
if(CY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
