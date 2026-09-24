# cmake/launchers.cmake — what every compile and link is run through: the machine-wide job pool
# (cmake/jobpool.cmake) and the compiler cache (cmake/ccache.cmake). Each module says why it exists
# and where it is off; this file only puts them together.
#
# A launcher is a property of how a compiler is invoked, so it is decided before the first target
# exists: `CMAKE_<LANG>_COMPILER_LAUNCHER` initialises a target's property when the target is
# created, and a launcher set afterwards does not reach it. They are ORDINARY variables, not cache
# entries, so turning `CY_CCACHE` or `CY_JOB_POOL` off, or uninstalling ccache, takes effect at the
# next configure instead of leaving a stale launcher remembered in the cache.
#
# A launcher the caller chose (`-D CMAKE_CXX_COMPILER_LAUNCHER=...`, or the environment variable of
# that name) wins over both: the caller asked for something specific.
#
# Links go through the pool with `--link`: a link is handed NO jobserver (GCC 13.3's `-flto` takes
# one from MAKEFLAGS when it finds one and deadlocks when its tokens run out — M11.c's seventh
# close), and the launcher instead holds as many slots as the link's own `-flto=N` names before the
# link starts. cmake/profiles.cmake fixes that N for the Shipping configuration (`CY_LTO_JOBS`),
# in place of the `-flto=auto` that would otherwise run one LTRANS job per core of the machine.

include(jobpool)
include(ccache)

cy_job_pool_command(cy_slot_command)
cy_ccache_command(cy_ccache_launcher "${cy_slot_command}")

if(cy_ccache_launcher)
    set(cy_compile_launcher ${cy_ccache_launcher})
else()
    set(cy_compile_launcher ${cy_slot_command})
endif()
set(cy_link_launcher "${cy_slot_command_LINK}")

foreach(cy_language IN ITEMS C CXX)
    if(DEFINED CMAKE_${cy_language}_COMPILER_LAUNCHER
       OR DEFINED ENV{CMAKE_${cy_language}_COMPILER_LAUNCHER})
        message(STATUS "Launchers: ${cy_language} keeps the compiler launcher the caller chose")
    elseif(cy_compile_launcher)
        set(CMAKE_${cy_language}_COMPILER_LAUNCHER ${cy_compile_launcher})
    endif()
    if(DEFINED CMAKE_${cy_language}_LINKER_LAUNCHER
       OR DEFINED ENV{CMAKE_${cy_language}_LINKER_LAUNCHER})
        message(STATUS "Launchers: ${cy_language} keeps the linker launcher the caller chose")
    elseif(cy_link_launcher)
        set(CMAKE_${cy_language}_LINKER_LAUNCHER ${cy_link_launcher})
    endif()
endforeach()
