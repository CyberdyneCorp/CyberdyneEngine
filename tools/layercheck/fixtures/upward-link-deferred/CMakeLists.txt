# Fixture (a'), the same violation with the declarations in the other order: the layer 0 target names
# a dependency that is not a target yet. CMake resolves link names at generate time, so this is legal
# CMake, and the check has to survive it — cy_add_module() queues the pair and verifies it once the
# top-level directory has been processed. Configure MUST fail.
cmake_minimum_required(VERSION 3.28)
project(cy_fixture_upward_link_deferred LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../support.cmake")

cy_add_module(NAME fx_core LAYER core
    SOURCES core.cpp
    PRIVATE_DEPENDENCIES fx_scene)

cy_add_module(NAME fx_scene LAYER scene SOURCES scene.cpp)
