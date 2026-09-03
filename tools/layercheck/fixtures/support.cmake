# fixtures/support.cmake — the smallest environment in which cy_add_module() is the real thing.
#
# The fixtures exercise cmake/module.cmake as it ships. Nothing here is a copy of engine machinery:
# the only stand-in is cy_compile_options, which cmake/compilers.cmake declares along with the
# compiler-version checks a fixture has no business running.

get_filename_component(CY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
list(APPEND CMAKE_MODULE_PATH "${CY_ROOT}/cmake")

add_library(cy_compile_options INTERFACE)
add_library(cy::compile-options ALIAS cy_compile_options)

include(module)
