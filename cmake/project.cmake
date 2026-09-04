# cmake/project.cmake — the project manifest, the graph built from it, and the layer rule applied to
# that graph rather than to one link at a time.
#
# Tasks 4.1, 4.2 and 4.4. M0 seeded `project-and-plugins` on its module half: cmake/modules.cmake
# reads modules/*/module.json and validates the graph *those* declare. This file adds the thing that
# requirement is actually about — a **project manifest** — and the two halves of enforcement that
# only exist once there is one:
#
#   * the project graph is validated by tools/project/, which owns the schema, the cycle check, the
#     undeclared-dependency check and the shipping/editor reachability rule. One implementation, in
#     the one language in this tree that has a JSON parser worth trusting;
#   * the *engine's* target graph is walked transitively here, so that the layer rule holds over the
#     whole graph and not only over the links cy_add_module() saw. cmake/module.cmake checks each
#     link as it is declared; a target_link_libraries() called after the fact, or a dependency
#     acquired through an interface target, is a link it never sees.
#
# WHERE THE MANIFEST LIVES. CY_PROJECT_MANIFEST names it; the default is project.json beside the
# top-level CMakeLists.txt. **The engine's own tree does not carry one yet** — see the note at
# cy_project_manifest() — and when there is none, the project header is rendered from the module
# manifests instead. That is still a declared graph: the records come from modules/*/module.json,
# not from what happens to be on disk.
#
# WHAT IT PRODUCES. <build>/generated/project/include/cy_project.h, the runtime's view of the graph.
# Generated rather than parsed at run time for the reason cy_modules.h is: one manifest parser, in a
# language that has one. src/core/config/ gives its tables names and types.
#
# Include it from a directory that is configured after cmake/modules.cmake has run —
# src/core/config/CMakeLists.txt does, because cy::core-config is what consumes the output. The
# natural home is `include(project)` in the top-level CMakeLists.txt beside include(modules); it is
# here instead only because the milestone's file ownership put the two files in different hands.

include_guard(GLOBAL)

set(CY_PROJECT_TOOL "${CMAKE_CURRENT_LIST_DIR}/../tools/project/project.py"
    CACHE INTERNAL "The program that validates a project manifest and renders cy_project.h")
set(CY_PROJECT_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated/project/include"
    CACHE INTERNAL "Where cy_project.h is written")

set(CY_PROJECT_MANIFEST "" CACHE FILEPATH
    "The project manifest. Empty means project.json beside the top-level CMakeLists.txt, if it exists.")

# --- Finding the manifest --------------------------------------------------------------------------
#
# An explicitly named manifest that does not exist is an error; an absent default is not. The
# difference matters: a typo in -DCY_PROJECT_MANIFEST must not silently fall back to "no project".

function(cy_project_manifest out)
    if(CY_PROJECT_MANIFEST)
        if(NOT EXISTS "${CY_PROJECT_MANIFEST}")
            message(FATAL_ERROR
                "CY_PROJECT_MANIFEST names '${CY_PROJECT_MANIFEST}', which does not exist.\n"
                "  A project manifest is project.json; tools/project/fixtures/valid/project.json is "
                "a complete example and tools/project/README.md documents the schema.")
        endif()
        set(${out} "${CY_PROJECT_MANIFEST}" PARENT_SCOPE)
        return()
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/project.json")
        set(${out} "${CMAKE_SOURCE_DIR}/project.json" PARENT_SCOPE)
        return()
    endif()
    set(${out} "" PARENT_SCOPE)
endfunction()

# The tool's own sources are configure inputs: a change to the schema must revalidate, or a manifest
# that stopped being valid keeps configuring from a stale answer.
function(_cy_project_configure_depends manifest)
    set(inputs "")
    if(manifest)
        list(APPEND inputs "${manifest}")
    endif()
    file(GLOB tool_sources "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/project/*.py")
    list(APPEND inputs ${tool_sources})
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${inputs})
endfunction()

# --- Validation ------------------------------------------------------------------------------------

# CMake drops an empty element from a COMMAND list rather than passing an empty argument, so a flag
# whose value happens to be empty would arrive at the tool with its value missing and be reported as
# a usage error. Options are therefore appended only when they carry something.
function(_cy_project_option list_variable flag value)
    if(value)
        set(${list_variable} ${${list_variable}} "${flag}" "${value}" PARENT_SCOPE)
    endif()
endfunction()

function(cy_validate_project_manifest manifest)
    set(arguments validate --manifest "${manifest}")
    _cy_project_option(arguments --engine-version "${PROJECT_VERSION}")
    _cy_project_option(arguments --platform "${CY_HOST_PLATFORM}")

    execute_process(
        COMMAND "${CY_PYTHON}" "${CY_PROJECT_TOOL}" ${arguments}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "The project graph was rejected.\n${errors}\n"
            "  The manifest is the authority on what this project contains: an undeclared "
            "dependency, a cycle and an upward layer dependency are build errors, not warnings.\n"
            "  tools/project/README.md documents the schema; `python3 tools/project/selftest.py` "
            "runs the fixtures that prove each rejection still fires.")
    endif()
    string(STRIP "${output}" output)
    message(STATUS "${output}")
endfunction()

# --- The generated header ----------------------------------------------------------------------------

function(_cy_project_module_arguments out)
    get_property(records GLOBAL PROPERTY CY_MODULE_RECORDS)
    set(arguments "")
    foreach(record IN LISTS records)
        # cmake/modules.cmake writes: name identifier state level level_index layer layer_index hot
        string(REPLACE " " ";" fields "${record}")
        list(GET fields 0 name)
        list(GET fields 2 state)
        list(GET fields 3 level)
        list(GET fields 4 level_index)
        list(GET fields 5 layer)
        list(GET fields 6 layer_index)
        list(GET fields 7 hot_reload)
        list(APPEND arguments --module
             "${name}|${layer}|${layer_index}|${level}|${level_index}|runtime|${hot_reload}|${state}")
    endforeach()
    set(${out} "${arguments}" PARENT_SCOPE)
endfunction()

function(cy_generate_project_header manifest)
    set(arguments emit-header --output "${CY_PROJECT_GENERATED_INCLUDE_DIR}/cy_project.h")
    _cy_project_option(arguments --engine-version "${PROJECT_VERSION}")
    _cy_project_option(arguments --platform "${CY_HOST_PLATFORM}")
    if(manifest)
        list(APPEND arguments --manifest "${manifest}")
    else()
        # No project manifest: the project is the engine, and its modules are the ones
        # cmake/modules.cmake discovered from their own manifests.
        _cy_project_module_arguments(module_arguments)
        _cy_project_option(arguments --project-name "${PROJECT_NAME}")
        _cy_project_option(arguments --project-version "${PROJECT_VERSION}")
        list(APPEND arguments ${module_arguments})
    endif()

    execute_process(
        COMMAND "${CY_PYTHON}" "${CY_PROJECT_TOOL}" ${arguments}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Rendering cy_project.h failed (exit ${result}):\n${errors}")
    endif()
    string(STRIP "${output}" output)
    message(STATUS "${output}")
endfunction()

# --- The layer rule over the whole target graph --------------------------------------------------------
#
# Task 4.2. cmake/module.cmake refuses an upward link at the moment cy_add_module() declares it. That
# is the check that fires in practice and it is worth keeping first, because it names the declaration
# that is wrong. It is not, however, the whole graph: it sees the arguments passed to cy_add_module()
# and nothing else, so a target_link_libraries() written afterwards, or a dependency inherited
# through an interface target, reaches a target without passing the check.
#
# This walks what CMake will actually link. Every target cy_add_module() declared is a root; the
# closure is followed through LINK_LIBRARIES and INTERFACE_LINK_LIBRARIES, unwrapping the
# $<LINK_ONLY:...> that a static library's private dependencies are recorded as. A target with no
# CY_LAYER — third-party, imported, interface — carries no layer and cannot violate a direction, but
# is still descended through, because what *it* links may.
#
# Deferred to the end of the top-level directory, where every target that will ever exist does.

function(_cy_project_link_closure target out)
    set(seen "")
    set(pending "${target}")
    while(pending)
        list(POP_FRONT pending current)
        if(current IN_LIST seen)
            continue()
        endif()
        list(APPEND seen "${current}")
        if(NOT TARGET "${current}")
            continue()
        endif()
        _cy_project_target_links("${current}" links)
        foreach(link IN LISTS links)
            if(NOT link IN_LIST seen)
                list(APPEND pending "${link}")
            endif()
        endforeach()
    endwhile()
    list(REMOVE_ITEM seen "${target}")
    set(${out} "${seen}" PARENT_SCOPE)
endfunction()

function(_cy_project_target_links target out)
    set(links "")
    foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(value "${target}" ${property})
        if(value)
            list(APPEND links ${value})
        endif()
    endforeach()
    list(TRANSFORM links REPLACE "^\\$<LINK_ONLY:(.+)>$" "\\1")
    # Anything still carrying a generator expression, a path, or a bare system library name is not a
    # target this check can resolve, and CMake reports an unresolved one itself.
    list(FILTER links EXCLUDE REGEX "[$/\\\\]")
    list(REMOVE_DUPLICATES links)
    set(${out} "${links}" PARENT_SCOPE)
endfunction()

function(cy_project_check_target_graph)
    get_property(targets GLOBAL PROPERTY CY_MODULES)
    set(checked 0)
    foreach(target IN LISTS targets)
        if(NOT TARGET "${target}")
            continue()
        endif()
        get_target_property(layer "${target}" CY_LAYER)
        if(NOT layer MATCHES "^[0-9]+$")
            continue()
        endif()
        math(EXPR checked "${checked} + 1")
        _cy_project_link_closure("${target}" closure)
        foreach(reached IN LISTS closure)
            _cy_project_check_reached("${target}" "${layer}" "${reached}")
        endforeach()
    endforeach()
    message(STATUS "Project graph: layer order holds over the transitive link closure of "
                   "${checked} target(s)")
endfunction()

function(_cy_project_check_reached target layer reached)
    if(NOT TARGET "${reached}")
        return()
    endif()
    get_target_property(reached_layer "${reached}" CY_LAYER)
    if(NOT reached_layer MATCHES "^[0-9]+$")
        return()
    endif()
    if(reached_layer GREATER layer)
        get_target_property(name "${target}" CY_LAYER_NAME)
        get_target_property(reached_name "${reached}" CY_LAYER_NAME)
        message(FATAL_ERROR
            "Layering violation in the project graph: target '${target}' (layer ${layer}, ${name}) "
            "reaches target '${reached}' (layer ${reached_layer}, ${reached_name}) through its "
            "transitive link closure.\n"
            "  cy_add_module() checks each link as it is declared; this checks what will actually "
            "be linked, so the dependency was added somewhere cy_add_module() did not see — a "
            "target_link_libraries() after the declaration, or an interface target that carries "
            "it.\n"
            "  A lower layer never depends on a higher one. Move the dependency down, or invert it.")
    endif()
endfunction()

# --- What including this file does -----------------------------------------------------------------

cy_project_manifest(_cy_project_manifest)
_cy_project_configure_depends("${_cy_project_manifest}")

if(_cy_project_manifest)
    cy_validate_project_manifest("${_cy_project_manifest}")
else()
    message(STATUS
        "Project graph: no project manifest; cy_project.h is rendered from the module manifests. "
        "Name one with -DCY_PROJECT_MANIFEST=<path>.")
endif()

cy_generate_project_header("${_cy_project_manifest}")

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_project_check_target_graph)
