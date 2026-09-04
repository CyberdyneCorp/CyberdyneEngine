# A tree whose targets are declared correctly and then wired incorrectly.
#
# cy_add_module() checks each link it is *given*. This fixture gives it none: fx_core is declared
# with no dependencies and acquires one afterwards, through a bare target_link_libraries(), which is
# a call cy_add_module() never sees. cy_project_check_target_graph() walks what CMake will actually
# link, so it is what catches this — task 4.2, "layering enforcement extended from targets to the
# project graph".
#
# Configured, never built: the check under test is a configure-time one.

cmake_minimum_required(VERSION 3.28)
project(cy_fixture_target_graph_violation LANGUAGES CXX VERSION 1.0.0)

include("${CMAKE_CURRENT_LIST_DIR}/../support.cmake")

cy_add_module(NAME fx_scene LAYER scene SOURCES src/scene.cpp)
cy_add_module(NAME fx_core  LAYER core  SOURCES src/core.cpp)

# The violation. Not an argument to cy_add_module(), so the per-link check never sees it.
target_link_libraries(fx_core PRIVATE fx_scene)

include(project)
