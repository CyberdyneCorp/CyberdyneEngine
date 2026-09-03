# benchmarks/ — performance benchmarks, and the one way a benchmark binary is declared.
#
# Task 4.1.5. A benchmark here is not a test: CTest does not run it, because a threshold checked on
# a laptop under load is a threshold that will be switched off within a month. `just test-bench`
# runs the binaries, writes results.json, and hands it to benchmarks/tools/compare.py, which is
# where a number becomes a pass or a failure.
#
# Runners are written to ${CMAKE_BINARY_DIR}/benchmarks/ so that the recipe can find every one of
# them without a manifest to keep in step.

set(CY_BENCHMARK_OUTPUT_DIR "${CMAKE_BINARY_DIR}/benchmarks")

# Declare one benchmark binary.
#
#   cy_add_benchmark(NAME <name> SOURCES <file>... [DEPENDENCIES <target>...])
#
# The target and the executable are both cy_bench_<name>.
function(cy_add_benchmark)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAME" "SOURCES;DEPENDENCIES")

    if(NOT arg_NAME OR NOT arg_SOURCES)
        message(FATAL_ERROR "cy_add_benchmark: NAME and SOURCES are required.")
    endif()
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "cy_add_benchmark(${arg_NAME}): unrecognised arguments: "
            "${arg_UNPARSED_ARGUMENTS}")
    endif()

    # LAYER tools, as for a test: a benchmark measures the engine from outside it.
    cy_add_module(
        NAME    cy_bench_${arg_NAME}
        LAYER   tools
        TYPE    EXECUTABLE
        SOURCES ${arg_SOURCES}
        PRIVATE_DEPENDENCIES cy::bench-harness ${arg_DEPENDENCIES})

    set_target_properties(cy_bench_${arg_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CY_BENCHMARK_OUTPUT_DIR}")
endfunction()

add_subdirectory(harness)
add_subdirectory(micro)
