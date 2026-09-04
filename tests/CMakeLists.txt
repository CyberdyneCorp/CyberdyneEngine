# tests/ — the test taxonomy, and the one way a test binary is declared.
#
# Tasks 4.1.1 and 4.1.2. `testing-and-quality` fixes the taxonomy: where each kind of test lives and
# what it may cost. That table is data here rather than prose — cy_add_test() reads it for the
# per-test budget it compiles into the binary and for the timeout it gives CTest, so a suite cannot
# be declared without a budget, and a budget cannot drift away from the specification without this
# file changing.
#
# enable_testing() is called by the top-level CMakeLists.txt, which is where CTest requires it; do
# not call it again here.

# --- The taxonomy ---------------------------------------------------------------------------------
#
#   kind | per-test budget (ns) | CTest timeout for the suite (s) | recipe
#
# The budget is per test case, measured as that case's own CPU time, and enforced inside the binary
# (cy::test::BudgetGuard — see its header for why CPU time rather than wall clock). The timeout is
# per suite, in wall clock, and is enforced by CTest: it is what stops a hung test from holding the
# run open, and for the unit suite it is also the "under a minute" requirement, checked rather than
# hoped for.

set(CY_TEST_BUDGET_unit        1000000)      # 1 ms
set(CY_TEST_BUDGET_integration 1000000000)   # 1 s
set(CY_TEST_BUDGET_smoke       30000000000)  # 30 s
# render/: 5 s per test, which is tests/render/README.md's own number. A case here creates a device,
# submits real work and waits for the GPU, and it does so several times — the conventions suite
# renders one frame per assertion because a frame is how you ask the device a question.
set(CY_TEST_BUDGET_render      5000000000)   # 5 s

set(CY_TEST_TIMEOUT_unit        60)
set(CY_TEST_TIMEOUT_integration 300)
set(CY_TEST_TIMEOUT_smoke       300)
set(CY_TEST_TIMEOUT_render      300)

# A test source that includes doctest directly has stepped around the CY_TEST_CASE wrapper, and the
# wrapper is the whole reason doctest is an acceptable choice (design.md §5). Checked here, at the
# point a suite is declared, and again at run time by tests/integration/test_wrapper_seam.cpp, which sees
# a file added after the last configure.
function(_cy_test_reject_direct_doctest target)
    foreach(source IN LISTS ARGN)
        if(NOT IS_ABSOLUTE "${source}")
            set(source "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        endif()
        file(READ "${source}" contents)
        if(contents MATCHES "#[ \t]*include[ \t]*[<\"]doctest")
            message(FATAL_ERROR
                "${source}\n"
                "  includes doctest directly. Tests are written against the CY_TEST_CASE wrapper in "
                "cy/test/test.h, so that the framework stays replaceable — see design.md §5. "
                "Replace the include with <cy/test/test.h> and the DOCTEST_* macros with their CY_* "
                "equivalents.\n"
                "  Declared by cy_add_test(NAME ${target} ...).")
        endif()
    endforeach()
endfunction()

# Declare one test suite: a binary, and the CTest entry that runs it.
#
#   cy_add_test(NAME <name> KIND unit|integration|smoke SOURCES <file>...
#               [DEPENDENCIES <target>...] [DEFINITIONS <def>...])
#
# The target is cy_test_<kind>_<name> and the CTest name is <kind>.<name>, so `ctest -R '^unit\.'`
# and `ctest -L unit` both select a suite the way a reader expects. Every suite carries its kind as a
# CTest label, which is how the graduated recipes in just/test.just select what they run.
function(cy_add_test)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAME;KIND" "SOURCES;DEPENDENCIES;DEFINITIONS")

    if(NOT arg_NAME OR NOT arg_KIND)
        message(FATAL_ERROR "cy_add_test: NAME and KIND are required.")
    endif()
    if(NOT DEFINED CY_TEST_BUDGET_${arg_KIND})
        message(FATAL_ERROR
            "cy_add_test(${arg_NAME}): KIND '${arg_KIND}' is not part of the taxonomy. It is one of "
            "unit, integration, smoke or render; determinism joins them at M9.")
    endif()
    if(NOT arg_SOURCES)
        message(FATAL_ERROR "cy_add_test(${arg_NAME}): SOURCES is required.")
    endif()
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "cy_add_test(${arg_NAME}): unrecognised arguments: "
            "${arg_UNPARSED_ARGUMENTS}")
    endif()

    _cy_test_reject_direct_doctest("${arg_NAME}" ${arg_SOURCES})

    set(target "cy_test_${arg_KIND}_${arg_NAME}")

    # LAYER tools: a test consumes the engine rather than sitting in it, so it may link any layer.
    # That is the same position tools/layercheck/layercheck.py gives tests/ for the include check.
    cy_add_module(
        NAME    ${target}
        LAYER   tools
        TYPE    EXECUTABLE
        SOURCES ${arg_SOURCES}
        PRIVATE_DEFINITIONS CY_TEST_BUDGET_NS=${CY_TEST_BUDGET_${arg_KIND}}ULL ${arg_DEFINITIONS}
        PRIVATE_DEPENDENCIES cy::test-harness ${arg_DEPENDENCIES})

    add_test(NAME ${arg_KIND}.${arg_NAME} COMMAND ${target}
             WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(${arg_KIND}.${arg_NAME} PROPERTIES
        LABELS "${arg_KIND}"
        TIMEOUT ${CY_TEST_TIMEOUT_${arg_KIND}})
endfunction()

add_subdirectory(harness)
add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(smoke)
# render/ was empty until M3, which is the milestone its README named. It holds section 5's
# conventions-on-a-device suite, and task 6.3's golden images join it.
add_subdirectory(render)

# determinism/ holds a README and nothing else: it is the suite whose subject does not exist yet, and
# an empty CMakeLists.txt would only be a place for a placeholder test to accumulate. It is added
# when simulation lands at M9.
