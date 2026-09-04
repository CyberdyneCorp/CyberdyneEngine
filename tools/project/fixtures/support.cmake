# The smallest environment in which cy_add_module() and cmake/project.cmake are the real things.
#
# Included by the target-graph fixtures. Nothing here is a copy of engine machinery: the compile
# options come from cmake/compilers.cmake itself, and the only stand-in is CY_PYTHON, which
# cmake/features.cmake sets after finding the interpreter and which a fixture has no business
# rediscovering along with the compiler-version checks that surround it.
#
# It is a fragment rather than a CMakeLists.txt of its own because both fixtures need it and neither
# is a subdirectory of the other.

get_filename_component(CY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
list(APPEND CMAKE_MODULE_PATH "${CY_ROOT}/cmake")

include(compilers)
cy_define_compile_options()

find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
set(CY_PYTHON "${Python3_EXECUTABLE}" CACHE INTERNAL "The interpreter the generators run under")

include(module)
