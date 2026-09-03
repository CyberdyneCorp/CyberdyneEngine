# Fixture (a): a target at layer 0 links a target at layer 4. Configure MUST fail, naming both
# targets and both layers.
cmake_minimum_required(VERSION 3.28)
project(cy_fixture_upward_link LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../support.cmake")

cy_add_module(NAME fx_scene LAYER scene SOURCES scene.cpp)

cy_add_module(NAME fx_core LAYER core
    SOURCES core.cpp
    PRIVATE_DEPENDENCIES fx_scene)
