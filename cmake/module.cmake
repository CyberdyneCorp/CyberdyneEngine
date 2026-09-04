# cmake/module.cmake — cy_add_module(), the one way an engine target is declared, and the
# configure-time layer check that makes the layer order a build failure rather than a review comment.
#
# Task 1.3.1. See design.md §1, engine-architecture "Layered architecture", and project-and-plugins
# "Architectural layering is enforced" — the last of which requires that violations fail the build,
# "not merely be reported".
#
# Two checks cover the rule, because target dependencies alone do not catch everything. This file is
# the first: it sees every link a target declares. tools/layercheck/ is the second: it sees an
# #include that reaches upward through a path CMake was never told about.
#
# A bare add_library or add_executable in the engine tree is a lint failure
# (tools/layercheck/layercheck.py), because it is a target that opted out of this check.

include_guard(GLOBAL)

# --- The layer table ------------------------------------------------------------------------------
#
# design.md §1. The index is the whole rule: a target may link a target whose index is less than or
# equal to its own, and nothing else. `platform` is accepted as a spelling of `backends` so that a
# reader in platform/desktop-sdl3/ does not have to remember that platform/ is layer 3.

# Two shapes of the same table. The parallel lists are the vocabulary cmake/modules.cmake reads when
# it resolves a module manifest's `layer` field — it defers to whatever this file published, because
# there must be exactly one layer order in the build. The CY_LAYER_INDEX_<name> variables below are
# this file's own lookup: a name is a variable suffix, so resolving one is not a list search.
set(CY_LAYER_NAMES   core ecs servers backends platform scene rendering runtime abi editor tools)
set(CY_LAYER_INDICES 0    1   2       3        3        4     4         5       6   7      7)

# Written out rather than derived, because a diagnostic that lists `platform` between `backends` and
# `scene` without saying what it is teaches the reader the wrong table.
set(CY_LAYER_NAMES_READABLE
    "core 0, ecs 1, servers 2, backends 3 (also spelled platform), scene 4 (also spelled "
    "rendering), runtime 5, abi 6, editor 7, tools 7")
string(REPLACE ";" "" CY_LAYER_NAMES_READABLE "${CY_LAYER_NAMES_READABLE}")

set(CY_LAYER_INDEX_core     0)  # src/core/
set(CY_LAYER_INDEX_ecs      1)  # src/ecs/
set(CY_LAYER_INDEX_servers  2)  # src/servers/
set(CY_LAYER_INDEX_backends 3)  # src/backends/, platform/
set(CY_LAYER_INDEX_scene    4)  # src/scene/
set(CY_LAYER_INDEX_runtime  5)  # src/runtime/
set(CY_LAYER_INDEX_abi      6)  # src/abi/
set(CY_LAYER_INDEX_editor   7)  # editor/
set(CY_LAYER_INDEX_tools    7)  # tools/
set(CY_LAYER_INDEX_platform 3)  # a spelling of backends, not a layer of its own
# src/rendering/ sits beside src/scene/: both are above the backends and below the runtime, and
# neither may see the other. A spelling rather than a layer of its own, for the same reason
# `platform` is one — a reader in src/rendering/graph/ should not have to remember that the layer
# they are at is called `scene`.
set(CY_LAYER_INDEX_rendering 4)

# What a layer index is called in a diagnostic. Keyed by index rather than by name, so that a module
# declaring LAYER 7 and one declaring LAYER editor read identically in the same message.
set(CY_LAYER_LABEL_0 "core")
set(CY_LAYER_LABEL_1 "ecs")
set(CY_LAYER_LABEL_2 "servers")
set(CY_LAYER_LABEL_3 "backends")
set(CY_LAYER_LABEL_4 "scene")
set(CY_LAYER_LABEL_5 "runtime")
set(CY_LAYER_LABEL_6 "abi")
set(CY_LAYER_LABEL_7 "editor/tools")

# Resolve a layer to its index. Sets <out> to the index and <out>_NAME to the label.
#
# A name or an index: `LAYER core` and `LAYER 0` are the same declaration. Both spellings are in the
# specifications — engine-architecture's table names the layers, design.md §1 numbers them — and
# rejecting either would only teach contributors which of the two tables this file was written from.
function(cy_layer_index layer out)
    string(TOLOWER "${layer}" name)
    if(name MATCHES "^[0-9]+$")
        set(index "${name}")
    elseif(DEFINED CY_LAYER_INDEX_${name})
        set(index "${CY_LAYER_INDEX_${name}}")
    endif()
    if(NOT DEFINED index OR NOT DEFINED CY_LAYER_LABEL_${index})
        message(FATAL_ERROR
            "cy_add_module: unknown layer '${layer}'.\n"
            "  Layers, lowest first: ${CY_LAYER_NAMES_READABLE}\n"
            "  A layer may be given by name or by index; 'platform' is accepted as a spelling of "
            "'backends' (layer 3).")
    endif()
    set(${out} "${index}" PARENT_SCOPE)
    set(${out}_NAME "${CY_LAYER_LABEL_${index}}" PARENT_SCOPE)
endfunction()

# --- The layer check ------------------------------------------------------------------------------

# Fail if `consumer`, at layer `consumer_layer`, links `dep` at a higher one.
#
# A dependency that is not yet a target is queued rather than ignored: CMake resolves link names at
# generate time, so a target may legitimately be linked before it is declared, and skipping those
# would leave a hole in the check exactly where declaration order is unusual.
function(_cy_check_link consumer consumer_layer dep)
    if(NOT TARGET "${dep}")
        set_property(GLOBAL APPEND PROPERTY CY_LAYER_PENDING_LINKS "${consumer}\t${dep}")
        _cy_arm_deferred_layer_check()
        return()
    endif()

    get_target_property(dep_layer "${dep}" CY_LAYER)
    if(NOT dep_layer MATCHES "^[0-9]+$")
        # Not declared by cy_add_module(): a third-party, imported or interface target. It has no
        # layer, so there is no direction to violate. Note that layer 0 is a valid value and a false
        # one to CMake's if(), which is why this tests the string rather than its truth.
        return()
    endif()

    if(dep_layer GREATER consumer_layer)
        get_target_property(consumer_name "${consumer}" CY_LAYER_NAME)
        get_target_property(dep_name "${dep}" CY_LAYER_NAME)
        message(FATAL_ERROR
            "Layering violation: target '${consumer}' (layer ${consumer_layer}, ${consumer_name}) "
            "links target '${dep}' (layer ${dep_layer}, ${dep_name}).\n"
            "  A lower layer never depends on a higher one. Either move the dependency down, or "
            "invert it: have '${dep}' register itself with an interface '${consumer}' owns.\n"
            "  Layers, lowest first: ${CY_LAYER_NAMES_READABLE}")
    endif()
endfunction()

# Arrange for the queued links to be checked once the whole tree has been processed, and do it only
# once. Deferring to the top-level directory means every add_subdirectory() has run by then, so every
# target that will ever exist does.
function(_cy_arm_deferred_layer_check)
    get_property(armed GLOBAL PROPERTY CY_LAYER_CHECK_ARMED)
    if(armed)
        return()
    endif()
    set_property(GLOBAL PROPERTY CY_LAYER_CHECK_ARMED TRUE)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_verify_pending_layer_links)
endfunction()

# Check the links that named a target which did not exist yet. Called automatically; it is a normal
# function rather than a private one so that a project embedding this file can force the pass early.
function(cy_verify_pending_layer_links)
    get_property(pending GLOBAL PROPERTY CY_LAYER_PENDING_LINKS)
    foreach(entry IN LISTS pending)
        string(REPLACE "\t" ";" parts "${entry}")
        list(GET parts 0 consumer)
        list(GET parts 1 dep)
        if(NOT TARGET "${dep}")
            # Still not a target: a system library name, or a name CMake will itself report as
            # unresolved at generate time. Either way it carries no layer.
            continue()
        endif()
        get_target_property(consumer_layer "${consumer}" CY_LAYER)
        _cy_check_link("${consumer}" "${consumer_layer}" "${dep}")
    endforeach()
    set_property(GLOBAL PROPERTY CY_LAYER_PENDING_LINKS "")
endfunction()

# --- cy_add_module --------------------------------------------------------------------------------
#
#   cy_add_module(
#       NAME                 <target>                    # required
#       LAYER                <core|ecs|servers|backends|platform|scene|runtime|abi|editor|tools>
#       [TYPE                STATIC|SHARED|OBJECT|INTERFACE|EXECUTABLE]   # default STATIC
#       [ALIAS               cy::<name>]
#       [SOURCES             <file>...]
#       [PUBLIC_INCLUDE_DIRS <dir>...]   [PRIVATE_INCLUDE_DIRS <dir>...]
#       [PUBLIC_DEFINITIONS  <def>...]   [PRIVATE_DEFINITIONS  <def>...]
#       [PUBLIC_DEPENDENCIES <target>...][PRIVATE_DEPENDENCIES <target>...])
#
# PUBLIC is the interface a dependent inherits; PRIVATE is implementation and does not leak, which is
# what project-and-plugins' "Private dependencies do not leak" requires. Both are layer-checked: a
# dependency that is private is still a dependency.

# Argument validation. A macro, not a function, because it reads the arg_* variables
# cmake_parse_arguments() left in cy_add_module's own scope.
#
# The required arguments are tested with DEFINED rather than `if(NOT arg_X)`, because CMake reads a
# bare `0` as false: `LAYER 0` — the core layer, spelled the way design.md §1's table spells it —
# would otherwise be rejected as no layer at all. The fixture legal/ declares one module that way so
# that it stays rejected only when it should be.
macro(_cy_module_validate_arguments)
    if(NOT DEFINED arg_NAME OR arg_NAME STREQUAL "")
        message(FATAL_ERROR "cy_add_module: NAME is required.")
    endif()
    if(NOT DEFINED arg_LAYER OR arg_LAYER STREQUAL "")
        message(FATAL_ERROR "cy_add_module(${arg_NAME}): LAYER is required — every module declares "
            "its layer. Layers, lowest first: ${CY_LAYER_NAMES_READABLE}")
    endif()
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "cy_add_module(${arg_NAME}): unrecognised arguments: "
            "${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(arg_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "cy_add_module(${arg_NAME}): keywords given no value: "
            "${arg_KEYWORDS_MISSING_VALUES}")
    endif()
    if(NOT DEFINED arg_TYPE OR arg_TYPE STREQUAL "")
        set(arg_TYPE "STATIC")
    endif()
    if(NOT arg_TYPE MATCHES "^(STATIC|SHARED|OBJECT|INTERFACE|EXECUTABLE)$")
        message(FATAL_ERROR "cy_add_module(${arg_NAME}): TYPE '${arg_TYPE}' is not one of "
            "STATIC, SHARED, OBJECT, INTERFACE, EXECUTABLE.")
    endif()
    if(arg_TYPE STREQUAL "INTERFACE" AND (arg_PRIVATE_DEPENDENCIES OR arg_PRIVATE_INCLUDE_DIRS
                                          OR arg_PRIVATE_DEFINITIONS OR arg_SOURCES))
        message(FATAL_ERROR "cy_add_module(${arg_NAME}): an INTERFACE module has no private side "
            "and compiles nothing; it cannot take SOURCES or PRIVATE_* arguments.")
    endif()
endmacro()

# A misspelled keyword after SOURCES is swallowed by SOURCES rather than reported as unparsed —
# cmake_parse_arguments() has no way to know that PUBLIC_DEPS was meant to be a keyword. It still
# fails the configure, as "Cannot find source file: PUBLIC_DEPS", which names the token.
function(cy_add_module)
    set(one_value NAME LAYER TYPE ALIAS)
    set(multi_value SOURCES PUBLIC_INCLUDE_DIRS PRIVATE_INCLUDE_DIRS PUBLIC_DEFINITIONS
        PRIVATE_DEFINITIONS PUBLIC_DEPENDENCIES PRIVATE_DEPENDENCIES)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "${one_value}" "${multi_value}")

    _cy_module_validate_arguments()
    cy_layer_index("${arg_LAYER}" layer)

    if(NOT TARGET cy_compile_options)
        message(FATAL_ERROR
            "cy_add_module(${arg_NAME}): cy::compile-options does not exist. Every engine target "
            "links it for the language standard and the warning set; include(compilers) first.")
    endif()

    if(arg_TYPE STREQUAL "EXECUTABLE")
        add_executable(${arg_NAME} ${arg_SOURCES})
    elseif(arg_TYPE STREQUAL "INTERFACE")
        add_library(${arg_NAME} INTERFACE)
    else()
        add_library(${arg_NAME} ${arg_TYPE} ${arg_SOURCES})
    endif()

    # An INTERFACE target has one scope for everything; every other kind has two.
    set(public_scope PUBLIC)
    set(private_scope PRIVATE)
    if(arg_TYPE STREQUAL "INTERFACE")
        set(public_scope INTERFACE)
        set(private_scope INTERFACE)
    endif()

    set_target_properties(${arg_NAME} PROPERTIES CY_LAYER "${layer}" CY_LAYER_NAME "${layer_NAME}")
    set_property(GLOBAL APPEND PROPERTY CY_MODULES "${arg_NAME}")

    target_link_libraries(${arg_NAME} ${private_scope} cy::compile-options)
    _cy_module_apply(${arg_NAME} ${public_scope} "${arg_PUBLIC_INCLUDE_DIRS}"
        "${arg_PUBLIC_DEFINITIONS}" "${arg_PUBLIC_DEPENDENCIES}")
    _cy_module_apply(${arg_NAME} ${private_scope} "${arg_PRIVATE_INCLUDE_DIRS}"
        "${arg_PRIVATE_DEFINITIONS}" "${arg_PRIVATE_DEPENDENCIES}")

    foreach(dep IN LISTS arg_PUBLIC_DEPENDENCIES arg_PRIVATE_DEPENDENCIES)
        _cy_check_link("${arg_NAME}" "${layer}" "${dep}")
    endforeach()

    if(arg_ALIAS AND arg_TYPE STREQUAL "EXECUTABLE")
        add_executable(${arg_ALIAS} ALIAS ${arg_NAME})
    elseif(arg_ALIAS)
        add_library(${arg_ALIAS} ALIAS ${arg_NAME})
    endif()
endfunction()

# Attach one scope's usage requirements. Extracted so that cy_add_module() states the public and the
# private side in one line each rather than six near-identical if()s.
function(_cy_module_apply target scope include_dirs definitions dependencies)
    if(include_dirs)
        target_include_directories(${target} ${scope} ${include_dirs})
    endif()
    if(definitions)
        target_compile_definitions(${target} ${scope} ${definitions})
    endif()
    if(dependencies)
        target_link_libraries(${target} ${scope} ${dependencies})
    endif()
endfunction()
