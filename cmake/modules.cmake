# cmake/modules.cmake — discovery of modules/, the CY_MODULE_<NAME> options generated from the
# manifests found there, CY_EXTRA_MODULE_PATHS for out-of-tree modules, and the project-graph
# validation that makes an undeclared dependency, a cycle and a layer violation build errors.
#
# Tasks 1.5.1 to 1.5.4. `project-and-plugins` states the rule this file implements: **the project
# graph is authoritative**. A module is what its manifest says it is; its presence in a folder alone
# does not include it in the build, and a malformed or unknown manifest entry is reported rather
# than ignored.
#
# Two orderings, deliberately distinct and both carried by every module:
#   * **layer** constrains dependencies — a module may not depend on one above it;
#   * **registration level** (Core, Servers, Scene, Editor) orders initialisation.
# Neither implies the other. A `Scene`-level module may sit at the core layer.
#
# The top-level CMakeLists.txt includes this file last, because discovery uses cy_add_module() and
# the feature options.

include_guard(GLOBAL)

# --- The vocabulary ------------------------------------------------------------------------------
#
# Layer names and their indices are `engine-architecture`'s table. If cmake/module.cmake has already
# published its own list, that one wins: there must be exactly one layer order in the build, and a
# second copy here that disagreed would be worse than none.

if(NOT DEFINED CY_LAYER_NAMES)
    set(CY_LAYER_NAMES core ecs servers backends platform scene rendering runtime abi editor tools)
    set(CY_LAYER_INDICES 0   1   2       3        3        4     4         5       6   7      7)
endif()

# Registration levels, in initialisation order. `engine-architecture` fixes both the set and the
# order: modules at Core initialise before the display server exists, modules at Servers before the
# world does, and Editor modules only in a tools build.
set(CY_REGISTRATION_LEVELS Core Servers Scene Editor)

# `project-and-plugins` names the module types.
set(CY_MODULE_TYPES runtime editor developer server tool third-party)

set(CY_MODULE_PLATFORMS linux windows macos ios android visionos web)

set(CY_MODULE_MANIFEST_KEYS
    name description layer type registration_level
    public_dependencies private_dependencies
    default_enabled platforms hot_reload)

set(CY_EXTRA_MODULE_PATHS "" CACHE STRING
    "Extra directories searched for modules. A path may be a module or a directory of modules.")

# --- Module records ------------------------------------------------------------------------------
#
# Discovery runs in a function, so the modules it finds are kept in global properties rather than in
# variables that would not survive the return.

function(_cy_module_set name key value)
    set_property(GLOBAL PROPERTY "CY_MODULE_RECORD_${name}_${key}" "${value}")
endfunction()

function(_cy_module_get name key out)
    get_property(value GLOBAL PROPERTY "CY_MODULE_RECORD_${name}_${key}")
    set(${out} "${value}" PARENT_SCOPE)
endfunction()

function(cy_module_names out)
    get_property(names GLOBAL PROPERTY CY_MODULE_NAMES)
    set(${out} "${names}" PARENT_SCOPE)
endfunction()

# The option, the C identifier and the target name are all derived from the module name, so that a
# reader who knows one knows the others.
function(cy_module_identifier name out)
    string(REPLACE "-" "_" identifier "${name}")
    set(${out} "${identifier}" PARENT_SCOPE)
endfunction()

function(cy_module_option name out)
    cy_module_identifier("${name}" identifier)
    string(TOUPPER "${identifier}" identifier)
    set(${out} "CY_MODULE_${identifier}" PARENT_SCOPE)
endfunction()

function(cy_module_target name out)
    cy_module_identifier("${name}" identifier)
    set(${out} "cy_module_${identifier}" PARENT_SCOPE)
endfunction()

# --- Manifest reading ----------------------------------------------------------------------------
#
# module.json, read with string(JSON), which CMake has had since 3.19 and which needs no dependency
# and no bespoke parser. JSON rather than TOML for exactly that reason — a hand-rolled TOML reader in
# CMake would be the only parser in the tree with no tests — and the same file is readable by the
# Python generators and, at M5, by the Rust editor.

function(_cy_manifest_error manifest message)
    string(REPLACE ";" ", " keys "${CY_MODULE_MANIFEST_KEYS}")
    message(FATAL_ERROR
        "${manifest}: ${message}\n"
        "  A module manifest declares: ${keys}.\n"
        "  modules/example-null/module.json is the template.")
endfunction()

function(_cy_manifest_member json manifest key expected_type out)
    string(JSON type ERROR_VARIABLE error TYPE "${json}" "${key}")
    if(NOT error STREQUAL "NOTFOUND")
        _cy_manifest_error("${manifest}" "missing required key '${key}'")
    endif()
    if(NOT type STREQUAL "${expected_type}")
        _cy_manifest_error("${manifest}" "key '${key}' is ${type}, expected ${expected_type}")
    endif()
    string(JSON value GET "${json}" "${key}")
    set(${out} "${value}" PARENT_SCOPE)
endfunction()

function(_cy_manifest_array json manifest key out)
    string(JSON type ERROR_VARIABLE error TYPE "${json}" "${key}")
    if(NOT error STREQUAL "NOTFOUND")
        _cy_manifest_error("${manifest}" "missing required key '${key}'")
    endif()
    if(NOT type STREQUAL "ARRAY")
        _cy_manifest_error("${manifest}" "key '${key}' is ${type}, expected ARRAY")
    endif()
    string(JSON count LENGTH "${json}" "${key}")
    set(values "")
    if(count GREATER 0)
        math(EXPR last "${count} - 1")
        foreach(index RANGE ${last})
            string(JSON element GET "${json}" "${key}" ${index})
            list(APPEND values "${element}")
        endforeach()
    endif()
    set(${out} "${values}" PARENT_SCOPE)
endfunction()

# Unknown keys are an error, not a warning: a manifest whose typo is ignored is a manifest that does
# not say what the build does.
function(_cy_manifest_reject_unknown_keys json manifest)
    string(JSON count LENGTH "${json}")
    if(count EQUAL 0)
        return()
    endif()
    math(EXPR last "${count} - 1")
    foreach(index RANGE ${last})
        string(JSON key MEMBER "${json}" ${index})
        if(NOT key IN_LIST CY_MODULE_MANIFEST_KEYS)
            _cy_manifest_error("${manifest}" "unknown key '${key}'")
        endif()
    endforeach()
endfunction()

function(_cy_manifest_enum value allowed manifest key)
    if(NOT value IN_LIST allowed)
        string(REPLACE ";" ", " readable "${allowed}")
        _cy_manifest_error("${manifest}" "${key} is '${value}'; expected one of: ${readable}")
    endif()
endfunction()

function(_cy_layer_index layer out)
    list(FIND CY_LAYER_NAMES "${layer}" position)
    list(GET CY_LAYER_INDICES ${position} index)
    set(${out} "${index}" PARENT_SCOPE)
endfunction()

# Read one manifest and record what it declares. The directory is passed as well as the file,
# because a module's sources are found relative to it.
function(_cy_read_manifest directory)
    set(manifest "${directory}/module.json")
    file(READ "${manifest}" json)

    _cy_manifest_reject_unknown_keys("${json}" "${manifest}")
    _cy_manifest_member("${json}" "${manifest}" name        STRING  name)
    _cy_manifest_member("${json}" "${manifest}" description STRING  description)
    _cy_manifest_member("${json}" "${manifest}" layer       STRING  layer)
    _cy_manifest_member("${json}" "${manifest}" type        STRING  type)
    _cy_manifest_member("${json}" "${manifest}" registration_level STRING level)
    _cy_manifest_member("${json}" "${manifest}" default_enabled BOOLEAN default_enabled)
    _cy_manifest_member("${json}" "${manifest}" hot_reload      BOOLEAN hot_reload)
    _cy_manifest_array("${json}" "${manifest}" public_dependencies  public_dependencies)
    _cy_manifest_array("${json}" "${manifest}" private_dependencies private_dependencies)
    _cy_manifest_array("${json}" "${manifest}" platforms            platforms)

    get_filename_component(folder "${directory}" NAME)
    if(NOT name STREQUAL folder)
        _cy_manifest_error("${manifest}"
            "declares the name '${name}' but sits in a directory called '${folder}'")
    endif()
    _cy_manifest_enum("${layer}" "${CY_LAYER_NAMES}" "${manifest}" layer)
    _cy_manifest_enum("${type}" "${CY_MODULE_TYPES}" "${manifest}" type)
    _cy_manifest_enum("${level}" "${CY_REGISTRATION_LEVELS}" "${manifest}" registration_level)
    foreach(platform IN LISTS platforms)
        _cy_manifest_enum("${platform}" "${CY_MODULE_PLATFORMS}" "${manifest}" platforms)
    endforeach()

    get_property(known GLOBAL PROPERTY CY_MODULE_NAMES)
    if(name IN_LIST known)
        _cy_module_get("${name}" DIRECTORY first)
        message(FATAL_ERROR
            "Two modules are called '${name}':\n  ${first}\n  ${directory}\n"
            "  Module names are the project graph's identifiers and must be unique.")
    endif()

    _cy_layer_index("${layer}" layer_index)
    list(FIND CY_REGISTRATION_LEVELS "${level}" level_index)

    _cy_module_set("${name}" DIRECTORY   "${directory}")
    _cy_module_set("${name}" DESCRIPTION "${description}")
    _cy_module_set("${name}" LAYER       "${layer}")
    _cy_module_set("${name}" LAYER_INDEX "${layer_index}")
    _cy_module_set("${name}" TYPE        "${type}")
    _cy_module_set("${name}" LEVEL       "${level}")
    _cy_module_set("${name}" LEVEL_INDEX "${level_index}")
    _cy_module_set("${name}" PUBLIC_DEPENDENCIES  "${public_dependencies}")
    _cy_module_set("${name}" PRIVATE_DEPENDENCIES "${private_dependencies}")
    _cy_module_set("${name}" PLATFORMS   "${platforms}")
    _cy_module_set("${name}" DEFAULT_ENABLED "${default_enabled}")
    _cy_module_set("${name}" HOT_RELOAD  "${hot_reload}")
    set_property(GLOBAL APPEND PROPERTY CY_MODULE_NAMES "${name}")

    # The manifest is an input to configuration, so changing it must reconfigure.
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}"
        APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest}")
endfunction()

# --- Discovery ------------------------------------------------------------------------------------
#
# A search path is either a module — it has a manifest — or a directory of modules. Both shapes are
# accepted so that CY_EXTRA_MODULE_PATHS can name one out-of-tree module or a directory of them, and
# in either case they build exactly as in-tree modules do.

function(_cy_collect_module_directories root out)
    if(EXISTS "${root}/module.json")
        set(${out} "${root}" PARENT_SCOPE)
        return()
    endif()
    if(NOT IS_DIRECTORY "${root}")
        message(FATAL_ERROR
            "CY_EXTRA_MODULE_PATHS names '${root}', which is not a directory.")
    endif()
    file(GLOB entries LIST_DIRECTORIES true "${root}/*")
    set(found "")
    foreach(entry IN LISTS entries)
        if(IS_DIRECTORY "${entry}" AND EXISTS "${entry}/module.json")
            list(APPEND found "${entry}")
        elseif(IS_DIRECTORY "${entry}" AND EXISTS "${entry}/CMakeLists.txt")
            message(FATAL_ERROR
                "${entry} has a CMakeLists.txt but no module.json, so it is not a module.\n"
                "  The manifest is what makes a directory a module: `project-and-plugins` requires "
                "the project graph to be declared, not inferred from the filesystem.")
        endif()
    endforeach()
    list(SORT found)
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

# Editor and tool modules are gated on the option that decides whether that code is built at all, so
# that `build-system-and-platforms`' separation — editor-only code is not linked into a shipped
# runtime — follows from the graph rather than from remembering to exclude it.
#
# The gate reads the module's **layer** and **type**, never its registration level: an `Editor`-level
# module may be ordinary runtime code that happens to initialise last.
function(_cy_module_gate name out)
    _cy_module_get("${name}" LAYER layer)
    _cy_module_get("${name}" TYPE type)
    if(layer STREQUAL "editor" OR type STREQUAL "editor")
        set(${out} CY_BUILD_EDITOR PARENT_SCOPE)
    elseif(layer STREQUAL "tools" OR type STREQUAL "tool")
        set(${out} CY_BUILD_TOOLS PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()

function(_cy_declare_module_option name)
    cy_module_option("${name}" option)
    _cy_module_get("${name}" DEFAULT_ENABLED default_enabled)
    _cy_module_get("${name}" DESCRIPTION description)
    _cy_module_get("${name}" PLATFORMS platforms)
    _cy_module_gate("${name}" gate)

    set(default "${default_enabled}")
    if(NOT CY_HOST_PLATFORM IN_LIST platforms)
        set(default OFF)
    endif()
    if(gate AND NOT ${gate})
        set(default OFF)
    endif()

    option(${option} "${description}" ${default})

    if(NOT ${option})
        return()
    endif()
    if(NOT CY_HOST_PLATFORM IN_LIST platforms)
        string(REPLACE ";" ", " readable "${platforms}")
        message(FATAL_ERROR
            "${option} is ON, but the module '${name}' supports ${readable} and this is "
            "${CY_HOST_PLATFORM}.\n  Disable it for this platform: -D${option}=OFF")
    endif()
    if(gate AND NOT ${gate})
        _cy_module_get("${name}" LAYER layer)
        _cy_module_get("${name}" TYPE type)
        message(FATAL_ERROR
            "${option} is ON, but the module '${name}' is ${type} code at the ${layer} layer and "
            "${gate} is OFF.\n  Build that code:       -D${gate}=ON\n"
            "  or disable the module: -D${option}=OFF")
    endif()
endfunction()

function(_cy_host_platform out)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(${out} linux PARENT_SCOPE)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(${out} windows PARENT_SCOPE)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(${out} macos PARENT_SCOPE)
    else()
        message(FATAL_ERROR
            "CMAKE_SYSTEM_NAME is '${CMAKE_SYSTEM_NAME}', which is not a supported platform.\n"
            "  Supported: Linux, Windows, Darwin. Planned: iOS, Android, visionOS, Web.")
    endif()
endfunction()

function(cy_discover_modules)
    _cy_host_platform(host)
    set(CY_HOST_PLATFORM "${host}" CACHE INTERNAL "The platform being built for, in manifest terms")

    set(roots "")
    if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/modules")
        list(APPEND roots "${CMAKE_SOURCE_DIR}/modules")
    endif()
    list(APPEND roots ${CY_EXTRA_MODULE_PATHS})
    set(directories "")
    foreach(root IN LISTS roots)
        _cy_collect_module_directories("${root}" found)
        list(APPEND directories ${found})
    endforeach()

    foreach(directory IN LISTS directories)
        _cy_read_manifest("${directory}")
    endforeach()

    cy_module_names(names)
    foreach(name IN LISTS names)
        _cy_declare_module_option("${name}")
    endforeach()

    cy_validate_module_graph()
    _cy_record_modules_for_generation()

    list(LENGTH names count)
    cy_enabled_modules(enabled)
    string(REPLACE ";" " " readable "${enabled}")
    message(STATUS "Modules: ${count} discovered, enabled: ${readable}")
endfunction()

function(cy_enabled_modules out)
    cy_module_names(names)
    set(enabled "")
    foreach(name IN LISTS names)
        cy_module_option("${name}" option)
        if(${option})
            list(APPEND enabled "${name}")
        endif()
    endforeach()
    set(${out} "${enabled}" PARENT_SCOPE)
endfunction()

# Modules in initialisation order: registration level, then name. This is the order they are added
# to the build and the order the generated table records.
function(cy_enabled_modules_in_registration_order out)
    cy_enabled_modules(enabled)
    set(keyed "")
    foreach(name IN LISTS enabled)
        _cy_module_get("${name}" LEVEL_INDEX level_index)
        list(APPEND keyed "${level_index}|${name}")
    endforeach()
    list(SORT keyed)
    set(ordered "")
    foreach(entry IN LISTS keyed)
        string(REPLACE "|" ";" fields "${entry}")
        list(GET fields 1 name)
        list(APPEND ordered "${name}")
    endforeach()
    set(${out} "${ordered}" PARENT_SCOPE)
endfunction()

function(_cy_record_modules_for_generation)
    cy_module_names(names)
    foreach(name IN LISTS names)
        cy_module_option("${name}" option)
        cy_module_identifier("${name}" identifier)
        _cy_module_get("${name}" LEVEL level)
        _cy_module_get("${name}" LEVEL_INDEX level_index)
        _cy_module_get("${name}" LAYER layer)
        _cy_module_get("${name}" LAYER_INDEX layer_index)
        _cy_module_get("${name}" HOT_RELOAD hot_reload)
        set(state OFF)
        if(${option})
            set(state ON)
        endif()
        set(hot OFF)
        if(hot_reload)
            set(hot ON)
        endif()
        set_property(GLOBAL APPEND PROPERTY CY_MODULE_RECORDS
            "${name} ${identifier} ${state} ${level} ${level_index} ${layer} ${layer_index} ${hot}")
    endforeach()
endfunction()

# --- Graph validation --------------------------------------------------------------------------------
#
# Task 1.5.4. Three failures, each a configure error naming what collided: a dependency on a module
# that does not exist or is disabled, a cycle, and a dependency that points upward through the layers.
#
# Manifest dependencies are dependencies on *other modules*. A module's dependencies on the engine's
# own layer targets are declared in its CMakeLists.txt through cy_add_module(), where the same layer
# rule applies to them.

function(_cy_module_dependencies name out)
    _cy_module_get("${name}" PUBLIC_DEPENDENCIES public)
    _cy_module_get("${name}" PRIVATE_DEPENDENCIES private)
    set(${out} ${public} ${private} PARENT_SCOPE)
endfunction()

function(_cy_check_dependency_exists name dependency)
    cy_module_names(known)
    if(dependency IN_LIST known)
        return()
    endif()
    _cy_module_get("${name}" DIRECTORY directory)
    message(FATAL_ERROR
        "Module '${name}' declares a dependency on '${dependency}', which is not a module.\n"
        "  Declared in ${directory}/module.json\n"
        "  Known modules: ${known}\n"
        "  A manifest dependency names another module. Engine targets are declared in the module's "
        "CMakeLists.txt through cy_add_module().")
endfunction()

function(_cy_check_dependency_enabled name dependency)
    cy_module_option("${name}" option)
    cy_module_option("${dependency}" dependency_option)
    if(NOT ${option})
        return()
    endif()
    if(NOT ${dependency_option})
        message(FATAL_ERROR
            "Module '${name}' is enabled and depends on '${dependency}', which is disabled.\n"
            "  Enable the dependency: -D${dependency_option}=ON\n"
            "  or disable the module: -D${option}=OFF")
    endif()
endfunction()

function(_cy_check_dependency_layer name dependency)
    _cy_module_get("${name}" LAYER_INDEX index)
    _cy_module_get("${dependency}" LAYER_INDEX dependency_index)
    if(dependency_index GREATER index)
        _cy_module_get("${name}" LAYER layer)
        _cy_module_get("${dependency}" LAYER dependency_layer)
        message(FATAL_ERROR
            "Layer violation: module '${name}' (layer ${layer}, ${index}) depends on module "
            "'${dependency}' (layer ${dependency_layer}, ${dependency_index}).\n"
            "  Dependencies point downward or within a layer, never upward.")
    endif()
endfunction()

# Depth-first search with the usual three colours, kept in a global property because CMake function
# scope does not survive the recursion. The stack is carried down so that a cycle is reported as the
# path that closed it rather than as the single edge that was noticed last.
function(_cy_visit_module name stack)
    get_property(state GLOBAL PROPERTY "CY_MODULE_VISIT_${name}")
    if(state STREQUAL "done")
        return()
    endif()
    if(state STREQUAL "open")
        list(APPEND stack "${name}")
        string(REPLACE ";" " -> " path "${stack}")
        message(FATAL_ERROR
            "Dependency cycle between modules: ${path}\n"
            "  Cycles are rejected: they have no initialisation order.")
    endif()

    set_property(GLOBAL PROPERTY "CY_MODULE_VISIT_${name}" "open")
    list(APPEND stack "${name}")
    _cy_module_dependencies("${name}" dependencies)
    foreach(dependency IN LISTS dependencies)
        _cy_check_dependency_exists("${name}" "${dependency}")
        _cy_check_dependency_layer("${name}" "${dependency}")
        _cy_check_dependency_enabled("${name}" "${dependency}")
        _cy_visit_module("${dependency}" "${stack}")
    endforeach()
    set_property(GLOBAL PROPERTY "CY_MODULE_VISIT_${name}" "done")
endfunction()

function(cy_validate_module_graph)
    cy_module_names(names)
    foreach(name IN LISTS names)
        set_property(GLOBAL PROPERTY "CY_MODULE_VISIT_${name}" "")
    endforeach()
    foreach(name IN LISTS names)
        _cy_visit_module("${name}" "")
    endforeach()
endfunction()

# --- Adding the enabled modules to the build -----------------------------------------------------------
#
# Called by modules/CMakeLists.txt. A disabled module's directory is never added, so its sources are
# not compiled and its dependencies are not fetched: `build-system-and-platforms` requires exclusion,
# not a runtime stub.

function(cy_add_module_subdirectories)
    if(NOT COMMAND cy_add_module)
        message(FATAL_ERROR
            "cy_add_module() is not defined, so no module could declare a target.\n"
            "  include(module) before include(modules): a module target is declared through "
            "cy_add_module() and no other way.")
    endif()

    cy_enabled_modules_in_registration_order(ordered)
    foreach(name IN LISTS ordered)
        _cy_module_get("${name}" DIRECTORY directory)
        cy_module_target("${name}" target)
        string(FIND "${directory}" "${CMAKE_SOURCE_DIR}" position)
        if(position EQUAL 0)
            add_subdirectory("${directory}")
        else()
            # Out of tree: the binary directory has to be named, and naming it under modules/ is what
            # makes an out-of-tree module build exactly as an in-tree one.
            add_subdirectory("${directory}" "${CMAKE_BINARY_DIR}/modules/${name}")
        endif()
        if(NOT TARGET ${target})
            message(FATAL_ERROR
                "Module '${name}' did not declare the target ${target}.\n"
                "  ${directory}/CMakeLists.txt must declare it with cy_add_module(NAME ${target} ...).\n"
                "  The name is derived from the module's, so the graph can be checked against the "
                "targets that were actually created.")
        endif()
    endforeach()

    cy_validate_module_targets()
endfunction()

# The manifest is authoritative, so what a module target links must be what its manifest declares.
# This is the graph-level half of "undeclared use SHALL be a build error, not a link-time accident";
# the source-level half is tools/layercheck (task 1.3.3).
function(cy_validate_module_targets)
    cy_enabled_modules(enabled)
    set(module_targets "")
    foreach(name IN LISTS enabled)
        cy_module_target("${name}" target)
        list(APPEND module_targets "${target}")
    endforeach()

    foreach(name IN LISTS enabled)
        _cy_check_module_links("${name}" "${module_targets}")
    endforeach()
endfunction()

function(_cy_check_module_links name module_targets)
    cy_module_target("${name}" target)
    _cy_module_get("${name}" PUBLIC_DEPENDENCIES public)
    _cy_module_get("${name}" PRIVATE_DEPENDENCIES private)

    set(declared_public "")
    foreach(dependency IN LISTS public)
        cy_module_target("${dependency}" dependency_target)
        list(APPEND declared_public "${dependency_target}")
    endforeach()
    set(declared_all ${declared_public})
    foreach(dependency IN LISTS private)
        cy_module_target("${dependency}" dependency_target)
        list(APPEND declared_all "${dependency_target}")
    endforeach()

    _cy_module_linked_targets("${target}" linked)
    foreach(link IN LISTS linked)
        if(link IN_LIST module_targets AND NOT link IN_LIST declared_all)
            message(FATAL_ERROR
                "Module '${name}' links ${link}, which its manifest does not declare.\n"
                "  Add it to public_dependencies or private_dependencies in the manifest, or stop "
                "linking it. The manifest is the authority on the project graph.")
        endif()
    endforeach()

    # A private dependency that reaches a dependent is not private. INTERFACE_LINK_LIBRARIES is what
    # dependents inherit, so a module target there that was declared private is the leak.
    _cy_module_exposed_targets("${target}" exposed)
    foreach(link IN LISTS exposed)
        if(link IN_LIST module_targets AND NOT link IN_LIST declared_public)
            message(FATAL_ERROR
                "Module '${name}' exposes ${link} through its interface, but does not declare it as "
                "a public dependency.\n"
                "  A private dependency is not transitively exposed to dependents: link it PRIVATE, "
                "or declare it in public_dependencies.")
        endif()
    endforeach()
endfunction()

function(_cy_target_property target property out)
    get_target_property(value ${target} ${property})
    if(NOT value)
        set(value "")
    endif()
    set(${out} "${value}" PARENT_SCOPE)
endfunction()

# Everything the target links, public and private alike. CMake records a static library's private
# dependencies as $<LINK_ONLY:dep>, so the wrapper is unwrapped to compare a plain target name
# against a plain target name.
function(_cy_module_linked_targets target out)
    _cy_target_property("${target}" LINK_LIBRARIES links)
    _cy_target_property("${target}" INTERFACE_LINK_LIBRARIES interface_links)
    list(APPEND links ${interface_links})
    list(TRANSFORM links REPLACE "^\\$<LINK_ONLY:(.+)>$" "\\1")
    list(REMOVE_DUPLICATES links)
    set(${out} "${links}" PARENT_SCOPE)
endfunction()

# What a dependent inherits. $<LINK_ONLY:dep> is in INTERFACE_LINK_LIBRARIES but carries none of the
# dependency's usage requirements — that is precisely how CMake keeps a private dependency private —
# so those entries are dropped rather than unwrapped. Unwrapping them here would report every
# correctly private dependency as a leak.
function(_cy_module_exposed_targets target out)
    _cy_target_property("${target}" INTERFACE_LINK_LIBRARIES links)
    list(FILTER links EXCLUDE REGEX "^\\$<LINK_ONLY:")
    set(${out} "${links}" PARENT_SCOPE)
endfunction()

# --- What including this file does -----------------------------------------------------------------
#
# Discovery, the options and the graph validation happen at include time, before any subdirectory is
# added, because CY_MODULE_<NAME> gates which dependencies are fetched and which sources exist. The
# targets themselves are added by modules/CMakeLists.txt, in registration order.

cy_discover_modules()
