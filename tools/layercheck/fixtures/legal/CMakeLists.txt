# The control. Every rule the other fixtures break, kept: the links point downward, the includes
# point downward, SDL appears only under platform/, and every target comes from cy_add_module().
# Configure MUST succeed and `layercheck.py --root legal` MUST be clean.
#
# It is a fixture rather than a comment because a check that fires on everything is as broken as one
# that fires on nothing, and only this catches that.
cmake_minimum_required(VERSION 3.28)
project(cy_fixture_legal LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../support.cmake")

# LAYER by index rather than by name: the two spellings are the same declaration, and `0` is the one
# CMake reads as false, so this is also the regression test for a required argument tested with
# if(NOT ...) instead of if(NOT DEFINED ...).
cy_add_module(NAME fx_core LAYER 0
    ALIAS fx::core
    SOURCES src/core/allocator.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/src")

# Layer 3. Declared with LAYER platform, the accepted spelling of backends, and linking downward.
cy_add_module(NAME fx_platform_sdl3 LAYER platform
    SOURCES platform/desktop-sdl3/display_server.cpp
    PRIVATE_DEPENDENCIES fx_core)

# Layer 5, linking layer 3 and layer 0. Both are below it, so both are legal.
cy_add_module(NAME fx_runtime LAYER runtime
    SOURCES src/runtime/runtime.cpp
    PUBLIC_DEPENDENCIES fx_core
    PRIVATE_DEPENDENCIES fx_platform_sdl3)

# Same layer, and a target declared after the link that names it: neither is a violation.
cy_add_module(NAME fx_runtime_host LAYER runtime
    SOURCES src/runtime/host.cpp
    PRIVATE_DEPENDENCIES fx_runtime)
