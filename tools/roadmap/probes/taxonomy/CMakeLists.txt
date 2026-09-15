# tools/roadmap/probes/taxonomy/ — ASK CMAKE WHAT cy_add_test() DOES, rather than read what it says.
#
# `m11a:testing-and-quality-at-working` re-checks the evidence `docs/roadmap/capability-matrix.md`
# argues the row's Working tier from, and its first three legs are about what tests/CMakeLists.txt
# PRODUCES: the per-case budget compiled into the binary, the kind as a CTest label, the taxonomy's
# CTest timeout. Until M11's second repair round those three legs were `fragment in taxonomy` — a
# substring search over the file — and the repair round demonstrated what that is worth: comment the
# three lines out, leave the words behind in the comment, and the criterion stayed GREEN over a
# cy_add_test() that compiled in no budget, set no label and set no timeout. That is the eighth
# instance of the defect `tools/roadmap/falsify.py` exists to end, and a ninth comment asking for
# care is not the fix.
#
# So this is the fix: a project CMake configures FOR REAL, with the taxonomy's own file included and
# cy_add_module() stubbed to record what it was handed. `add_subdirectory()` is stubbed to nothing
# so that the engine's suites are not declared — the subject is the FUNCTION, not the tree that
# calls it — and the probe declares one suite per kind the taxonomy names. What comes back is
# CMake's answer, twice over:
#
#   * `${CY_PROBE_REPORT}` — one `taxonomy|<kind>|<budget>|<timeout>` line per kind, read from the
#     CMake variables rather than from the file's text, and one `module|<target>|<definitions>` line
#     per suite, holding exactly the PRIVATE_DEFINITIONS cy_add_test() passed on.
#   * the generated `CTestTestfile.cmake` — the LABELS and TIMEOUT properties CTest will actually
#     see, written by CMake's generator and not by anything in this repository.
#
# LANGUAGES NONE, so no compiler is required and the configure costs about a tenth of a second: this
# runs on every pull request through `milestone-m11a`, and in `falsify.prove`'s sandboxed copy of
# the tracked tree, which has no build.
#
# Driven by `tools/roadmap/row_evidence.py`. Three variables come in on the command line:
#   CY_REPO          the repository root to read tests/CMakeLists.txt from
#   CY_PROBE_REPORT  the file to append the two record kinds to
#   CY_PROBE_SOURCE  a source file for the declared suites; written by the caller, never committed,
#                    because a .cpp under tools/ is a file every source gate would then scan

cmake_minimum_required(VERSION 3.24)
project(cy_taxonomy_probe LANGUAGES NONE)
enable_testing()

foreach(required CY_REPO CY_PROBE_REPORT CY_PROBE_SOURCE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "the taxonomy probe needs -D${required}=<path>")
    endif()
endforeach()

# The engine's suites are declared beside the modules they test, and declaring them here would need
# the whole engine tree. The subject is cy_add_test() itself.
function(add_subdirectory)
endfunction()

# The one call cy_add_test() makes that would need the engine's build system. Recording its
# arguments is how the budget reaches this probe: `PRIVATE_DEFINITIONS CY_TEST_BUDGET_NS=...` is
# what "compiles the budget into the binary" MEANS, and it is a fact about the call, not the file.
function(cy_add_module)
    set(_lists "SOURCES;PRIVATE_DEFINITIONS;PRIVATE_DEPENDENCIES")
    list(APPEND _lists "PUBLIC_DEFINITIONS" "PUBLIC_DEPENDENCIES")
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAME;LAYER;TYPE" "${_lists}")
    file(APPEND "${CY_PROBE_REPORT}" "module|${arg_NAME}|${arg_PRIVATE_DEFINITIONS}\n")
endfunction()

include("${CY_REPO}/tests/CMakeLists.txt")

# The kinds are whatever the taxonomy DEFINED, asked of CMake's variable set rather than matched out
# of the text: a kind only spelled in a comment defines no variable and appears nowhere here.
get_cmake_property(_variables VARIABLES)
list(SORT _variables)
set(_kinds "")
foreach(_variable IN LISTS _variables)
    if(_variable MATCHES "^CY_TEST_BUDGET_(.+)$")
        list(APPEND _kinds "${CMAKE_MATCH_1}")
    endif()
endforeach()

if(NOT _kinds)
    file(APPEND "${CY_PROBE_REPORT}" "empty|the taxonomy defines no CY_TEST_BUDGET_<kind> at all\n")
endif()

foreach(_kind IN LISTS _kinds)
    file(APPEND "${CY_PROBE_REPORT}"
         "taxonomy|${_kind}|${CY_TEST_BUDGET_${_kind}}|${CY_TEST_TIMEOUT_${_kind}}\n")
    cy_add_test(NAME probe_${_kind} KIND ${_kind} SOURCES "${CY_PROBE_SOURCE}")
endforeach()
