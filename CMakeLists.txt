# CyberdyneEngine — the top-level build.
#
# Tasks 1.2.1, 1.2.3 and 1.2.4. The build is target-based: every engine target is declared through
# cy_add_module() (cmake/module.cmake), which records its layer and refuses at configure time to
# link a target above it. There is no way to declare a target that opts out of that check.
#
# A first build is one command: `cmake --preset dev && cmake --build --preset dev`, or `just
# build-engine --profile dev`, which runs exactly those two.

cmake_minimum_required(VERSION 3.28)

# --- No in-source builds --------------------------------------------------------------------------
# Checked before anything else, because by the time CMake has written a cache into the source tree
# the damage is already done and the remedy is manual.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not supported: they scatter build output through the source tree.\n"
        "  Remove the CMakeCache.txt and CMakeFiles/ this attempt created, then use a preset:\n"
        "      cmake --preset dev\n"
        "  or the recipe that wraps it:\n"
        "      just build-engine --profile dev")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# The configuration set must exist before project() so that the generator sees it.
include(profiles)
cy_declare_build_configurations()

project(CyberdyneEngine
    VERSION 0.0.0
    DESCRIPTION "A C++20 game engine"
    HOMEPAGE_URL "https://github.com/cyberdyne-engine/CyberdyneEngine"
    LANGUAGES C CXX)

cy_apply_build_configurations()

# Ninja is the default generator; the presets in CMakePresets.json select it. Any generator works,
# but build times and the CI numbers are quoted against Ninja.
message(STATUS "CyberdyneEngine ${PROJECT_VERSION} — ${CMAKE_GENERATOR}, ${CMAKE_BUILD_TYPE}")

# --- Language contract ----------------------------------------------------------------------------
#
# C++20, with exceptions and RTTI disabled engine-wide. `engine-architecture` fixes both: fallible
# operations return cy::Expected<T, Error> and programmer errors use the CY_ASSERT macros, so there
# is nothing for an exception to carry and nothing for dynamic_cast to ask.
#
# These are directory-scope options rather than usage requirements of cy::compile-options, because
# the contract is not opt-in: it must hold for every translation unit compiled in this tree,
# including one added by a file that forgot to link the interface target. A third-party subdirectory
# that genuinely requires exceptions clears them for its own scope in cmake/dependencies.cmake.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    # CMake seeds CMAKE_CXX_FLAGS with /EHsc. Leaving it there and appending /EHs-c- makes the
    # compiler report D9025 for every translation unit, so remove it rather than override it.
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/EHs-c-> $<$<COMPILE_LANGUAGE:CXX>:/GR->)
    add_compile_definitions($<$<COMPILE_LANGUAGE:CXX>:_HAS_EXCEPTIONS=0>)
else()
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions> $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Written unconditionally: editors and clang-tidy read it, and a developer who has not opted in is
# exactly the one who will be told to "just enable compile_commands.json".
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors in engine code" ON)

include(compilers)
cy_check_compiler_support()
cy_define_compile_options()

# --- Dedicated server -----------------------------------------------------------------------------
#
# Task 1.2.4. Orthogonal to configuration: any of the four configurations may be built as a dedicated
# server. The rule `build-system-and-platforms` states is a *link-time* one — the renderer, VFX, UI,
# client audio and the editor are excluded from the link rather than disabled at runtime, and the
# build fails if a client-only subsystem is linked in.
#
# The check below is a stub, because at M0 there is no renderer to exclude. It is written now so that
# the rule has a home when the first client-only target appears at M3.

option(CY_DEDICATED_SERVER "Build a dedicated server: no renderer, VFX, UI, client audio or editor" OFF)

# Targets that a dedicated server may not link. Populated as each client-only subsystem lands; the
# renderer is the first, at M3.
set(CY_CLIENT_ONLY_TARGETS "" CACHE INTERNAL "Targets excluded from a dedicated server build")

function(cy_check_dedicated_server_link)
    if(NOT CY_DEDICATED_SERVER)
        return()
    endif()
    # TODO(M3): walk the runtime target's transitive link set and fail naming any target in
    # CY_CLIENT_ONLY_TARGETS. Until a renderer exists there is nothing to exclude, and a check that
    # can only pass is worse than no check: it reads as coverage it does not have.
    if(CY_CLIENT_ONLY_TARGETS)
        message(FATAL_ERROR
            "cy_check_dedicated_server_link() is still the M0 stub, but client-only targets are "
            "now declared: ${CY_CLIENT_ONLY_TARGETS}. Implement the link-set walk.")
    endif()
endfunction()

if(CY_DEDICATED_SERVER)
    add_compile_definitions(CY_DEDICATED_SERVER=1)
    message(STATUS "Dedicated server build: renderer, VFX, UI, client audio and editor excluded")
endif()

# --- Build modules ---------------------------------------------------------------------------------
#
# Include order is a dependency order. Each file's header comment names the task that fills it.
#
# features before sanitizers: cmake/features.cmake owns the declaration of CY_SANITIZE, and
# cmake/sanitizers.cmake reads it. Both before the tree is added, because the flags they select are
# usage requirements of every target declared below.
include(features)       # the CY_* option set, CY_SANITIZE, and the generated headers
include(sanitizers)     # CY_SANITIZE turned into flags, added to the options every target inherits
include(dependencies)   # FetchContent from deps/manifest.toml, gated by the feature options
include(module)         # cy_add_module() and the configure-time layer check
include(modules)        # discovery of modules/ and CY_EXTRA_MODULE_PATHS

# --- Tests ------------------------------------------------------------------------------------------
#
# enable_testing() has to be called in *this* directory: CTest only writes the top-level
# CTestTestfile.cmake that `ctest --preset <p>` reads if the top-level directory enabled testing, and
# an add_test() in tests/ is invisible without it. Called before the tree is added so that every
# subdirectory may register tests.
if(CY_BUILD_TESTS)
    enable_testing()
endif()

# --- The tree ---------------------------------------------------------------------------------------
#
# CY_BUILD_TESTS, CY_BUILD_TOOLS and CY_BUILD_EDITOR are declared by cmake/features.cmake, included
# above. A disabled option excludes the directory rather than compiling something that does nothing:
# `build-system-and-platforms` requires exclusion, not a runtime stub.
#
# editor/ and bindings/ are absent here on purpose: Cargo owns the first and the Swift package
# manager the second, and the C++ build does not reimplement either. CY_BUILD_EDITOR gates the
# editor-layer modules (cmake/modules.cmake), not a subdirectory of this build.
add_subdirectory(src)
add_subdirectory(platform)
add_subdirectory(modules)

if(CY_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(CY_BUILD_TESTS)
    add_subdirectory(tests)
    add_subdirectory(benchmarks)
endif()

add_subdirectory(samples)

cy_check_dedicated_server_link()

# --- The profile spike ------------------------------------------------------------------------------
#
# Task 0.2: prove one profile end to end. A trivial target that reports, at run time, what the
# configuration actually gave it — the profile, the optimisation level, whether CY_DEVELOPMENT is
# defined and whether assertions survive — so that the claim "the profiles mean what the table says"
# is a program's output rather than a reading of the flags.
#
# It is the one bare add_executable in the tree, and it is deliberate: cy_add_module() does not exist
# until task 1.3.1, and this target must build before it. Delete this block, and the option, once
# samples/00-empty builds (task 3.6.1) — it is scaffolding, not a target anyone should depend on.

option(CY_BUILD_PROFILE_SPIKE "Build the M0 profile spike target (task 0.2)" ON)

if(CY_BUILD_PROFILE_SPIKE)
    set(spike_source "${CMAKE_CURRENT_BINARY_DIR}/spike/cy_profile_spike.cpp")
    file(WRITE "${spike_source}" [==[
// Generated by the top-level CMakeLists.txt. Task 0.2. Do not edit; do not commit.
#include <cassert>
#include <cstdio>

static_assert(__cplusplus >= 202002L, "CyberdyneEngine requires C++20");

int main() {
    std::printf("profile       %s\n", CY_SPIKE_PROFILE);
    std::printf("configuration %s\n", CY_SPIKE_CONFIG);
    std::printf("__cplusplus   %ld\n", static_cast<long>(__cplusplus));
#if defined(CY_DEVELOPMENT)
    std::printf("CY_DEVELOPMENT defined\n");
#else
    std::printf("CY_DEVELOPMENT undefined\n");
#endif
#if defined(NDEBUG)
    std::printf("NDEBUG        defined\n");
#else
    std::printf("NDEBUG        undefined\n");
#endif
#if defined(__cpp_exceptions)
    std::printf("exceptions    ENABLED -- the -fno-exceptions contract is not being applied\n");
    return 1;
#else
    std::printf("exceptions    disabled\n");
#endif
#if defined(__cpp_rtti) || (defined(_CPPRTTI) && _CPPRTTI)
    std::printf("rtti          ENABLED -- the -fno-rtti contract is not being applied\n");
    return 1;
#else
    std::printf("rtti          disabled\n");
#endif
    // assert() is live exactly when NDEBUG is absent, which is exactly the two configurations the
    // profile table marks "assertions on". Firing one would abort, so report the state instead.
#if defined(NDEBUG)
    std::printf("assertions    off\n");
#else
    std::printf("assertions    on\n");
#endif
    return 0;
}
]==])

    # A multi-configuration generator has no single build type, so the profile is only known per
    # configuration there; the spike is run from a single-configuration build.
    cy_profile_for_configuration("${CMAKE_BUILD_TYPE}" spike_profile)
    if(NOT spike_profile)
        set(spike_profile "per-configuration")
    endif()

    add_executable(cy_profile_spike "${spike_source}")
    target_link_libraries(cy_profile_spike PRIVATE cy::compile-options)
    target_compile_definitions(cy_profile_spike PRIVATE
        CY_SPIKE_PROFILE="${spike_profile}"
        CY_SPIKE_CONFIG="$<CONFIG>")
endif()
