# The same shape, wired the right way round: the higher layer links the lower one. The transitive
# check must accept it, so that a check which rejected everything would fail here rather than look
# like it was working.

cmake_minimum_required(VERSION 3.28)
project(cy_fixture_target_graph_legal LANGUAGES CXX VERSION 1.0.0)

include("${CMAKE_CURRENT_LIST_DIR}/../support.cmake")

cy_add_module(NAME fx_core  LAYER core  SOURCES src/core.cpp)
cy_add_module(NAME fx_scene LAYER scene SOURCES src/scene.cpp)

target_link_libraries(fx_scene PRIVATE fx_core)

include(project)
