# cmake/ccache.cmake — a compiler cache, used when this machine has one and never required.
#
# WHY. A milestone ledger builds a few dozen trees, and every one of them recompiles from nothing
# when its directory has been removed or when a header every translation unit includes has been
# touched without being changed. M11.c's fifth close took 7 h 28 m because `build/m11c-final` had
# been reaped and every tree was built from empty. ccache answers a recompilation whose inputs are
# byte-identical from its store instead of from the compiler.
#
# WHY A MODULE AND NOT CMakePresets.json. A preset can only set a launcher unconditionally, so a
# preset naming ccache breaks the configure on every machine and CI runner that does not have it,
# and it would not reach a tree configured with `cmake -B` and no preset. This module yields a
# launcher ONLY WHEN `ccache` IS FOUND, and none at all when:
#   * `-D CY_CCACHE=OFF` was given, or the `CI` environment variable is set and `CY_CCACHE` was not
#     turned on explicitly — a runner's cache is thrown away with the runner, so there it would
#     only cost a hash per translation unit;
#   * the compiler is MSVC, where ccache needs `/Z7` in place of the `/Zi` CMake defaults to, or the
#     generator is not Ninja or Makefiles.
# cmake/launchers.cmake installs what this returns, and leaves a launcher the caller chose alone.
#
# WHAT IT DOES NOT CHANGE. No compile flag is added or rewritten. In particular `base_dir` is NOT
# set: ccache would then hand the compiler RELATIVE paths, `__FILE__` would stop starting with
# `${CMAKE_SOURCE_DIR}/`, and the `-fmacro-prefix-map` in cmake/compilers.cmake would stop removing
# the build machine's layout from diagnostic site names. The cost is that two trees at DIFFERENT
# paths do not share entries (their generated-header include paths and, with `-g`, their working
# directories differ anyway). A tree rebuilt AT THE SAME PATH — a reaped ledger tree, a
# `fresh_cache` matrix row, a header touched but not changed — is answered from the cache, and that
# is the case that costs hours. The sanitizer trees work unchanged: ccache hashes `-fsanitize=*` and
# the contents of an ignore list. Swift and Rust are not compiled through it (CMake has no Swift
# launcher; Cargo has its own incremental cache).
#
# THE STORE IS THE PROJECT'S OWN. The launcher passes `cache_dir` and `max_size` to ccache as
# KEY=VALUE settings, which override every configuration file, so this project's objects neither
# evict nor are evicted by any other project's, and the user's global ccache configuration is never
# edited. `CY_CCACHE_DIR` and `CY_CCACHE_MAX_SIZE` override the defaults; the store's statistics are
# `ccache --dir <store> --show-stats` (it prints the global max_size, not this one). cmake/README.md
# has the figures behind the default size.
#
# WITH THE JOB POOL. When cmake/jobpool.cmake yields a slot command it goes to ccache as
# `prefix_command` and `prefix_command_cpp`, so a compile takes a machine slot only when ccache
# actually runs the compiler or the preprocessor: a cache hit costs a hash and takes no slot.

set(CY_CCACHE_DEFAULT_MAX_SIZE "60G")

# Sets `out_var` to the ccache launcher, or to "" when ccache is off or absent. `slot_command` is the
# job pool's program for a compile, or "".
function(cy_ccache_command out_var slot_command)
    set(${out_var} "" PARENT_SCOPE)
    if(DEFINED ENV{CI} AND NOT DEFINED CACHE{CY_CCACHE})
        set(default OFF)
    else()
        set(default ON)
    endif()
    option(CY_CCACHE "Compile through ccache when it is installed" ${default})
    set(CY_CCACHE_DIR "" CACHE PATH
        "ccache store for this project (default: <XDG cache>/cyberdyne-ccache)")
    set(CY_CCACHE_MAX_SIZE "${CY_CCACHE_DEFAULT_MAX_SIZE}" CACHE STRING
        "Size limit of this project's ccache store")

    if(NOT CY_CCACHE)
        message(STATUS "Compiler cache: off (CY_CCACHE=OFF)")
        return()
    endif()
    if(MSVC OR NOT CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
        message(STATUS "Compiler cache: off (${CMAKE_GENERATOR} / ${CMAKE_CXX_COMPILER_ID})")
        return()
    endif()
    find_program(CY_CCACHE_PROGRAM ccache)
    if(NOT CY_CCACHE_PROGRAM)
        message(STATUS "Compiler cache: off (ccache not found)")
        return()
    endif()

    set(store "${CY_CCACHE_DIR}")
    if(store STREQUAL "")
        if(NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
            set(store "$ENV{XDG_CACHE_HOME}/cyberdyne-ccache")
        elseif(NOT "$ENV{HOME}" STREQUAL "")
            set(store "$ENV{HOME}/.cache/cyberdyne-ccache")
        endif()
    endif()
    set(launcher "${CY_CCACHE_PROGRAM}")
    if(NOT store STREQUAL "")
        list(APPEND launcher "cache_dir=${store}" "max_size=${CY_CCACHE_MAX_SIZE}")
    endif()

    # ccache looks every space-separated word of a prefix up as a program, so the pool's program
    # (one path, see cmake/jobpool.cmake) goes in only when its path has no space. Otherwise the
    # pool wraps ccache itself, and a hit takes a slot for its few milliseconds.
    if(slot_command AND NOT slot_command MATCHES " ")
        list(APPEND launcher "prefix_command=${slot_command}" "prefix_command_cpp=${slot_command}")
    elseif(slot_command)
        set(launcher "${slot_command}" ${launcher})
    endif()

    set(${out_var} "${launcher}" PARENT_SCOPE)
    message(STATUS "Compiler cache: ${CY_CCACHE_PROGRAM} (store ${store}, ${CY_CCACHE_MAX_SIZE})")
endfunction()
