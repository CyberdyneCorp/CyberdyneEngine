# src/core/reflect/ — layer 0: the opt-in type registry, and the persistent identity of types and
# fields that every artefact written from M1 onward encodes.
#
# Sections 1.1 and 1.2. Governed by `core-type-system`. The `TypeRegistry`, `TypeId` and `FieldId`,
# field attributes as strongly typed structures, and the runtime half of the identity manifest.
#
# The generator is not here: it is a tool-time program in tools/gen/, and this module is what its
# output registers against. Identifiers are opaque numbers read from the committed manifest, never
# derived from a name, a hash or an index (design.md §1).
#
# Reflection is control plane. Nothing on a per-entity or per-frame path may reach into it.
#
# --- Where the generated code lives, and why it is committed -----------------------------------------
#
# `identity/manifest.toml` is committed, and the C++ that encodes those identifiers is committed
# beside it in generated/. Three consequences, all of them wanted:
#
#   * Building the engine does not require libclang. A contributor who changes nothing annotated
#     pays nothing, and the ~65 ms no-op floor the M1 spike measured is not on everyone's every
#     build.
#   * An identity change is visible in review as a diff of both the manifest and the code that
#     encodes it, rather than as a number that appears in a build directory.
#   * `just generate-check` is a currency check over committed files, which is what makes staleness
#     a merge gate rather than a local surprise.
#
# The generator still runs as a build step when the frontend is present, so an edit to an annotated
# header regenerates within the build that compiles it — `core-type-system` requires generation be
# a build step and be incremental, and a committed artefact nobody regenerates goes stale.

find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)

set(cy_reflect_generator "${CMAKE_SOURCE_DIR}/tools/gen/reflect_gen.py")

# Sources are listed explicitly and never globbed, generated ones included: a file this list does
# not name is compiled by nothing, which reads in a review exactly like a file that passes.
set(cy_reflect_annotated_headers
    "${CMAKE_CURRENT_SOURCE_DIR}/include/cy/core/reflect/demo/types.h")

set(cy_reflect_attribute_schemas
    "${CMAKE_CURRENT_SOURCE_DIR}/reflect_attributes.toml")

set(cy_reflect_generated_sources
    generated/cy_reflect_generated.cpp
    generated/cy/core/reflect/demo/types.reflect.cpp)

cy_add_module(
    NAME cy_core_reflect
    LAYER core
    ALIAS cy::core-reflect
    TYPE STATIC
    SOURCES
        src/attributes.cpp
        src/control_plane.cpp
        src/registry.cpp
        src/serialize.cpp
        src/type_info.cpp
        ${cy_reflect_generated_sources}
    PUBLIC_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated"
    PUBLIC_DEPENDENCIES cy::core-base)

# --- The generator as a build step ------------------------------------------------------------------
#
# Detected rather than assumed. The bindings are pinned in deps/manifest.toml and the libclang
# library is the platform's, so a checkout without them still builds — it just cannot regenerate,
# and `just generate-check` is what catches an annotated header edited without one.

set(CY_REFLECT_PYTHON "${Python3_EXECUTABLE}" CACHE FILEPATH
    "The interpreter that runs the reflection generator; needs the pinned clang bindings")

execute_process(
    COMMAND "${CY_REFLECT_PYTHON}" "${cy_reflect_generator}" --probe
    RESULT_VARIABLE cy_reflect_probe_result
    OUTPUT_VARIABLE cy_reflect_probe_output
    ERROR_VARIABLE cy_reflect_probe_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)

if(cy_reflect_probe_result EQUAL 0)
    set(cy_reflect_frontend_default ON)
    message(STATUS "Reflection generator: ${cy_reflect_probe_output}")
else()
    set(cy_reflect_frontend_default OFF)
    message(STATUS
        "Reflection generator unavailable; compiling the committed metadata in "
        "src/core/reflect/generated/ instead. Editing an annotated header will not regenerate it.")
endif()

option(CY_REFLECT_GENERATE "Regenerate reflection metadata as a build step"
       ${cy_reflect_frontend_default})

# What the generator must be run with, published for the recipes. The same list drives the build
# step below, so `just generate-headers`, `just generate-check` and `just quality-identity` cannot
# be run against a different set of headers than the build compiles — which is the drift a second
# copy of this list in the justfile would guarantee.
#
# Written relative to the source root, one argument per line, because that is the form a recipe can
# read into an argument vector without quoting rules, and because a build directory's absolute paths
# have no business in a file other tools read.
set(cy_reflect_inputs "${CMAKE_BINARY_DIR}/generated/cy_reflect_inputs.txt")
set(cy_reflect_argument_lines "")
foreach(argument IN ITEMS
        "--manifest" "identity/manifest.toml"
        "--output-dir" "src/core/reflect/generated"
        "--include" "src/core/base/include"
        "--include" "src/core/reflect/include")
    string(APPEND cy_reflect_argument_lines "${argument}
")
endforeach()
foreach(schema IN LISTS cy_reflect_attribute_schemas)
    file(RELATIVE_PATH relative "${CMAKE_SOURCE_DIR}" "${schema}")
    string(APPEND cy_reflect_argument_lines "--attributes
${relative}
")
endforeach()
foreach(annotated IN LISTS cy_reflect_annotated_headers)
    file(RELATIVE_PATH relative "${CMAKE_SOURCE_DIR}" "${annotated}")
    string(APPEND cy_reflect_argument_lines "--header
${relative}
")
endforeach()
string(APPEND cy_reflect_argument_lines "--engine-version
${PROJECT_VERSION}
")
file(WRITE "${cy_reflect_inputs}" "${cy_reflect_argument_lines}")

if(CY_REFLECT_GENERATE)
    set(cy_reflect_stamp "${CMAKE_CURRENT_BINARY_DIR}/reflect_generated.stamp")

    # The generator's own sources are dependencies: a change to the emitter changes every output,
    # and a build that did not notice would compile last week's metadata.
    file(GLOB cy_reflect_generator_sources CONFIGURE_DEPENDS
         "${CMAKE_SOURCE_DIR}/tools/gen/reflect/*.py")

    add_custom_command(
        OUTPUT "${cy_reflect_stamp}"
        COMMAND "${CY_REFLECT_PYTHON}" "${cy_reflect_generator}"
                --source-root "${CMAKE_SOURCE_DIR}"
                --manifest "${CMAKE_SOURCE_DIR}/identity/manifest.toml"
                --output-dir "${CMAKE_CURRENT_SOURCE_DIR}/generated"
                --include "${CMAKE_SOURCE_DIR}/src/core/base/include"
                --include "${CMAKE_CURRENT_SOURCE_DIR}/include"
                --attributes ${cy_reflect_attribute_schemas}
                --header ${cy_reflect_annotated_headers}
                --engine-version "${PROJECT_VERSION}"
                --cache "${CMAKE_CURRENT_BINARY_DIR}/reflect_cache.json"
                --stamp "${cy_reflect_stamp}"
                --quiet
        DEPENDS
            "${cy_reflect_generator}"
            ${cy_reflect_generator_sources}
            ${cy_reflect_annotated_headers}
            ${cy_reflect_attribute_schemas}
            "${CMAKE_SOURCE_DIR}/identity/manifest.toml"
        COMMENT "Generating reflection metadata"
        VERBATIM)

    add_custom_target(cy_core_reflect_generate DEPENDS "${cy_reflect_stamp}")
    add_dependencies(cy_core_reflect cy_core_reflect_generate)
endif()

if(CY_BUILD_TESTS)
    # cy_add_test() is declared by tests/CMakeLists.txt, which the top-level adds *after* src/, and
    # a subdirectory may not be created during deferred execution. So the declarations are a
    # function defined now and called once the whole tree has been processed. See tests/declare.cmake.
    include("${CMAKE_CURRENT_SOURCE_DIR}/tests/declare.cmake")
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_reflect_declare_tests)
endif()
