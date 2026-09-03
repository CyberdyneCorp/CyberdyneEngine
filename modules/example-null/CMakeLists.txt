# modules/example-null/ — the module template.
#
# This file is reached only when CY_MODULE_EXAMPLE_NULL is ON: cmake/modules.cmake adds a module's
# directory only if its option is on, so disabling a module excludes its sources rather than
# compiling a runtime stub.
#
# The target name is not free. cmake/modules.cmake derives it from the module name —
# example-null -> cy_module_example_null — so that the graph it validates can be checked against the
# targets that were actually created. A module that declares a differently named target is a
# configure error.
#
# The layer is given twice on purpose: here, where cy_add_module() enforces it against every link,
# and in module.json, where the project graph is validated against it. The two must agree.

cy_add_module(
    NAME  cy_module_example_null
    LAYER core
    ALIAS cy::module-example-null
    SOURCES
        src/example_null.cpp
    PUBLIC_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/include")

# A module's dependencies on other modules are declared in module.json and passed here as
# PUBLIC_DEPENDENCIES or PRIVATE_DEPENDENCIES. This module has none; cmake/modules.cmake fails the
# configure if a target ever links a module its manifest does not declare.
