# src/core/plugins/ — layer 0: what a plugin is, how a set of them is resolved, and the eight
# phases they are brought up through.
#
# M11.b section 2, and it is the rung's FIRST editor section rather than one of its last.
# `editor-architecture`'s "Specialised editors" requirement is normative about the order — each
# built-in editor "SHALL be a plugin using the same panel and undo infrastructure as user plugins,
# so the extension API is exercised by the engine's own tooling" — so an extension API written after
# its first consumers would be an API shaped by what they had already done.
#
# WHY IT IS NOT PART OF cy::core-config. `config/` is the project graph as the GENERATOR renders it:
# `<cy_project.h>` is written at configure time by tools/project/project.py and read with no parsing
# and no allocation. A plugin manifest is read at run time, from a file the project did not compile,
# and it allocates. Putting a run-time reader in the module whose whole claim is that it does not
# read anything would make that claim false for both halves.
#
# LAYER core, and it depends on cy::core-values for `Name` and cy::core-serialize for the text form.
# It deliberately does NOT depend on cy::core-reflect: type ownership here is by NAME, and the
# instance count comes from a caller-supplied counter, because `cy::ecs` is above this layer and a
# host that guessed zero would permit exactly the unload the requirement exists to refuse.
cy_add_module(
    NAME cy_core_plugins
    LAYER core
    ALIAS cy::core-plugins
    TYPE STATIC
    SOURCES
        src/host.cpp
        src/plugin.cpp
        src/resolve.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::core-values
    PRIVATE_DEPENDENCIES cy::core-serialize)

if(CY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
