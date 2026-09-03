# cmake/features.cmake — the CY_* feature option set, its dependency validation, and the generated
# headers cy_features.h and cy_modules.h.
#
# Tasks 1.4.1, 1.4.2 and 1.4.3. `build-system-and-platforms` fixes the option set; design.md §8 says
# why the whole of it is declared at M0, defaulting OFF, rather than one option per subsystem as the
# subsystems arrive: the generated-header machinery, the dependency validation and the rule that
# disabling excludes sources rather than stubbing at runtime each get written once, and the last is
# the one that quietly does not happen when retrofitted.
#
# The top-level CMakeLists.txt includes this file after cmake/sanitizers.cmake and before
# cmake/dependencies.cmake, because a dependency is fetched only when the feature that gates it is
# enabled.
#
# The generated headers are written at the end of configuration, not here, because cy_modules.h
# depends on the modules cmake/modules.cmake discovers, and discovery happens after this file is
# included. See cy_generate_headers() at the bottom.

include_guard(GLOBAL)

# --- The option set ---------------------------------------------------------------------------
#
# One row per option: NAME|DEFAULT|description. Nearly everything defaults OFF because nearly
# everything does not exist yet; an option that defaults ON for a subsystem with no sources would
# make `#if defined(CY_UI)` a lie for six milestones. CY_BUILD_TESTS and CY_BUILD_TOOLS default ON
# because their subjects exist now and the gates depend on them.
#
# Descriptions may not contain a semicolon or a vertical bar: CMake splits lists on the first and
# this table on the second.

set(CY_FEATURE_OPTIONS
    "CY_BUILD_EDITOR|OFF|Build the editor application (the Rust workspace, from M5)"
    "CY_BUILD_TESTS|ON|Build the unit, integration and smoke test suites"
    "CY_BUILD_TOOLS|ON|Build the command-line tools and code generators"
    "CY_SCRIPTING|OFF|Swift scripting: the C ABI export table and the game module loader (M4)"
    "CY_PHYSICS|OFF|Physics simulation and PhysicsServer backends (M6)"
    "CY_NAVIGATION|OFF|Navigation meshes, path queries and NavigationServer (M8)"
    "CY_AI|OFF|Behaviour graphs, perception and the AI runtime (M8)"
    "CY_ML|OFF|Machine-learning inference nodes (M8)"
    "CY_ANIMATION|OFF|Skeletal animation, blending and the animation graph (M6)"
    "CY_AUDIO|OFF|The audio runtime and AudioServer backends (M7)"
    "CY_AUDIO_STEAM_AUDIO|OFF|Steam Audio spatial acoustics inside CY_AUDIO (M7)"
    "CY_UI|OFF|The retained-mode UI runtime (M9)"
    "CY_VFX|OFF|The VFX runtime, its compiler and its renderers (M7)"
    "CY_VIRTUAL_GEOMETRY|OFF|The virtualised geometry path inside the renderer (M10)"
    "CY_NETWORKING|OFF|Replication, transport and the network runtime (M9)"
    "CY_XR|OFF|Extended-reality sessions, tracking and stereo rendering (M11)"
    "CY_PROFILING|OFF|Tracy as a backend of the engine's own trace (M0 seam, M2 wiring)"
    "CY_RENDERER_VULKAN|OFF|The Vulkan RHI backend (M3)"
    "CY_RENDERER_METAL|OFF|The Metal RHI backend (M7)"
    "CY_RENDERER_D3D12|OFF|The Direct3D 12 RHI backend (M7)"
    CACHE INTERNAL "The CY_* feature options: NAME|DEFAULT|description")

# --- Feature dependencies ---------------------------------------------------------------------
#
# Each row is FEATURE|REQUIRED... — every named option must be on when the feature is on.
# `build-system-and-platforms` states two of these outright: CY_AI requires CY_NAVIGATION, and
# CY_AUDIO_STEAM_AUDIO gates spatial acoustics *within* CY_AUDIO.
#
# CY_AI deliberately does not require CY_ML. The specification's "AI without ML" scenario has AI
# building and running fully with only inference-bearing graphs failing to cook.

set(CY_FEATURE_REQUIRES_ALL
    "CY_AI|CY_NAVIGATION"
    "CY_AUDIO_STEAM_AUDIO|CY_AUDIO"
    CACHE INTERNAL "Feature dependencies: FEATURE|every option it requires")

# Each row is FEATURE|CANDIDATE... — at least one of the named options must be on. CY_VIRTUAL_GEOMETRY
# gates a path inside the renderer, so it needs a renderer, but not a particular one.
set(CY_FEATURE_REQUIRES_ANY
    "CY_VIRTUAL_GEOMETRY|CY_RENDERER_VULKAN CY_RENDERER_METAL CY_RENDERER_D3D12"
    CACHE INTERNAL "Feature dependencies: FEATURE|options of which at least one is required")

# --- Settings -----------------------------------------------------------------------------------
#
# CY_SANITIZE is in the option set `build-system-and-platforms` lists, but it carries a value rather
# than a state — which sanitizers, not whether. It is declared here so that the option set is
# complete in one place and so that it reaches the generated header; cmake/sanitizers.cmake (task
# 4.2.3) turns the value into flags and must not redeclare it.

if(NOT DEFINED CY_SANITIZE)
    set(CY_SANITIZE "" CACHE STRING
        "Sanitizers to build with: a comma-separated subset of address,undefined,thread")
endif()
set_property(CACHE CY_SANITIZE PROPERTY STRINGS "" address undefined thread "address,undefined")

set(CY_SETTINGS CY_SANITIZE CACHE INTERNAL "Valued options that reach the generated headers")

# --- Declaration ---------------------------------------------------------------------------------

function(cy_declare_features)
    foreach(row IN LISTS CY_FEATURE_OPTIONS)
        string(REPLACE "|" ";" fields "${row}")
        # A semicolon anywhere in the table splits a row, because CMake lists are semicolon
        # separated. Caught here, where the row that did it can be named.
        list(LENGTH fields count)
        if(NOT count EQUAL 3)
            message(FATAL_ERROR
                "cmake/features.cmake: malformed feature row '${row}'.\n"
                "  A row is NAME|DEFAULT|description and may not contain a semicolon.")
        endif()
        list(GET fields 0 name)
        list(GET fields 1 default)
        list(GET fields 2 description)
        option(${name} "${description}" ${default})
    endforeach()
endfunction()

# Every declared feature name, for callers that enumerate rather than test.
function(cy_feature_names out)
    set(names "")
    foreach(row IN LISTS CY_FEATURE_OPTIONS)
        string(REPLACE "|" ";" fields "${row}")
        list(GET fields 0 name)
        list(APPEND names "${name}")
    endforeach()
    set(${out} "${names}" PARENT_SCOPE)
endfunction()

# --- Validation -----------------------------------------------------------------------------------
#
# A missing dependency is a configure failure naming the option that is required and both ways out of
# it, because "CY_AI needs something" is a diagnostic the reader has to translate and "-DCY_NAVIGATION=ON"
# is one they can paste.

function(_cy_require_all feature required)
    foreach(dependency IN LISTS required)
        if(NOT ${dependency})
            message(FATAL_ERROR
                "${feature} is ON but requires ${dependency}, which is OFF.\n"
                "  Enable the dependency:  -D${dependency}=ON\n"
                "  or disable the feature: -D${feature}=OFF\n"
                "  Declared in cmake/features.cmake (CY_FEATURE_REQUIRES_ALL).")
        endif()
    endforeach()
endfunction()

function(_cy_require_any feature candidates)
    foreach(candidate IN LISTS candidates)
        if(${candidate})
            return()
        endif()
    endforeach()
    string(REPLACE ";" ", " readable "${candidates}")
    message(FATAL_ERROR
        "${feature} is ON but requires at least one of: ${readable}. All of them are OFF.\n"
        "  Enable one:             -D<option>=ON\n"
        "  or disable the feature: -D${feature}=OFF\n"
        "  Declared in cmake/features.cmake (CY_FEATURE_REQUIRES_ANY).")
endfunction()

function(_cy_validate_rows rows validator)
    foreach(row IN LISTS rows)
        string(REPLACE "|" ";" fields "${row}")
        list(GET fields 0 feature)
        list(GET fields 1 required)
        string(REPLACE " " ";" required "${required}")
        if(${feature})
            cmake_language(CALL ${validator} "${feature}" "${required}")
        endif()
    endforeach()
endfunction()

function(cy_validate_features)
    _cy_validate_rows("${CY_FEATURE_REQUIRES_ALL}" _cy_require_all)
    _cy_validate_rows("${CY_FEATURE_REQUIRES_ANY}" _cy_require_any)

    cy_feature_names(names)
    set(enabled "")
    foreach(name IN LISTS names)
        if(${name})
            list(APPEND enabled "${name}")
        endif()
    endforeach()
    string(REPLACE ";" " " enabled "${enabled}")
    message(STATUS "Features enabled: ${enabled}")
endfunction()

# --- Generated headers ------------------------------------------------------------------------------
#
# Task 1.4.3. cmake/features.cmake decides *what* is generated; tools/gen/generate_headers.py decides
# how it is rendered, and is a separate program so that the currency check (`just generate-check`) is
# the same code path as the write and cannot disagree with it. Generation is reproducible: the input
# is sorted, carries no timestamp, no path and no configuration name, and an unchanged output file is
# not rewritten.

# The generator is found relative to this file, not to CMAKE_SOURCE_DIR, so that the build modules
# keep working when they are included from somewhere else — which is exactly what the fixture
# projects in tools/gen/tests/ do.
set(CY_GENERATOR_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../tools/gen/generate_headers.py"
    CACHE INTERNAL "The generator that renders cy_features.h and cy_modules.h")
set(CY_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated/include"
    CACHE INTERNAL "Where cy_features.h and cy_modules.h are written")
set(CY_GENERATED_INPUT_FILE "${CMAKE_BINARY_DIR}/generated/cy_generated_inputs.txt"
    CACHE INTERNAL "The generator input written by cmake/features.cmake")

# Write only when the content changed: rewriting an unchanged file makes every configure invalidate
# every compilation that reads it.
function(_cy_write_if_changed path content)
    if(EXISTS "${path}")
        file(READ "${path}" existing)
        if(existing STREQUAL "${content}")
            return()
        endif()
    endif()
    file(WRITE "${path}" "${content}")
endfunction()

function(_cy_write_generator_input)
    set(lines
        "# CyberdyneEngine generated-header inputs."
        "# Written by cmake/features.cmake at configure time. Do not edit."
        "version 1")

    cy_feature_names(names)
    list(SORT names)
    foreach(name IN LISTS names)
        set(state OFF)
        if(${name})
            set(state ON)
        endif()
        list(APPEND lines "feature ${name} ${state}")
    endforeach()

    foreach(name IN LISTS CY_SETTINGS)
        list(APPEND lines "setting ${name} ${${name}}")
    endforeach()

    # Module records are produced by cmake/modules.cmake, already in the generator's field order.
    get_property(modules GLOBAL PROPERTY CY_MODULE_RECORDS)
    if(modules)
        list(SORT modules)
        foreach(record IN LISTS modules)
            list(APPEND lines "module ${record}")
        endforeach()
    endif()

    list(JOIN lines "\n" text)
    _cy_write_if_changed("${CY_GENERATED_INPUT_FILE}" "${text}\n")
endfunction()

function(cy_generate_headers)
    _cy_write_generator_input()

    execute_process(
        COMMAND "${CY_PYTHON}" "${CY_GENERATOR_SCRIPT}"
                --input "${CY_GENERATED_INPUT_FILE}"
                --output-dir "${CY_GENERATED_INCLUDE_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "Generating cy_features.h and cy_modules.h failed (exit ${result}):\n${errors}")
    endif()
    string(STRIP "${output}" output)
    message(STATUS "${output}")
endfunction()

# The generator is Python, which `just env-doctor` already requires. Finding it here rather than at
# the point of use means the absence is reported once, in the language of the environment check.
function(_cy_find_python)
    find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
    if(NOT Python3_Interpreter_FOUND)
        message(FATAL_ERROR
            "Python 3.9 or later is required to generate cy_features.h and cy_modules.h, and none "
            "was found.\n  Run `just env-doctor` for the toolchain report and the correction for "
            "this host.")
    endif()
    set(CY_PYTHON "${Python3_EXECUTABLE}" CACHE INTERNAL "The interpreter that runs tools/gen/")
endfunction()

# --- What including this file does -------------------------------------------------------------------

cy_declare_features()
cy_validate_features()
_cy_find_python()

# Every target compiled in this tree can include <cy_features.h> and <cy_modules.h>. The interface
# target is for targets declared elsewhere; the directory-scope include path covers every
# subdirectory added below, including out-of-tree modules added through CY_EXTRA_MODULE_PATHS.
add_library(cy_generated_headers INTERFACE)
add_library(cy::generated-headers ALIAS cy_generated_headers)
target_include_directories(cy_generated_headers INTERFACE "${CY_GENERATED_INCLUDE_DIR}")
include_directories("${CY_GENERATED_INCLUDE_DIR}")

# Deferred to the end of this directory's configuration: cy_modules.h records the modules discovered
# by cmake/modules.cmake, which runs after this file is included.
cmake_language(DEFER CALL cy_generate_headers)
