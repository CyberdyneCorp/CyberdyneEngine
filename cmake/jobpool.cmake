# cmake/jobpool.cmake — the machine-wide cap on concurrent compiles and links.
#
# WHY. The owner's rule is that at least two of the workstation's cores stay free WHILE COMPILING,
# MACHINE-WIDE: however many builds run at once, the compile jobs between them stay at or below
# cores - 2. A per-build `-j` cannot say that — the ledger builds up to eight trees at once, the
# build matrix several, and every agent its own — and Ninja 1.11 cannot share a jobserver between
# builds. So every compile and every link runs through tools/workflow/job_slot.py, which takes one
# slot of a pool of `tools/workflow/jobs.sh machine` flock(2) slots shared by every build of this
# user on this machine, and waits asleep while none is free. job_slot.py's docstring says what
# happens when builds overlap and why this is a lock-file pool rather than a named-pipe jobserver.
#
# WHERE IT IS OFF. A continuous-integration runner (`CI` set, unless `CY_JOB_POOL` is turned on
# explicitly) builds one tree on a machine nobody sits at, so there the pool would only cost a Python
# start per compile. Windows and the Xcode and Visual Studio generators have no flock(2) or no
# launcher; there it is off too, and `-j` is the only bound. `-D CY_JOB_POOL=OFF` turns it off.
#
# WHAT IT DOES NOT CHANGE. No compile or link flag. The launcher is part of each compile's command
# line, so turning the pool on or off makes Ninja recompile the tree once; with ccache in front
# (cmake/ccache.cmake) that recompile is answered from the cache.
#
# THE SLOT COUNT IS BAKED IN AT CONFIGURE TIME, from the same tools/workflow/jobs.sh `just _jobs`
# reads, rather than recomputed by every compile: one definition, and no fork per compile.
# `CY_RESERVED_CORES` at configure time changes it. Trees configured with different counts still
# share one pool, and the machine total never exceeds the largest count in use.

# Where the pool's two scripts are, taken from this file's own location rather than from
# CMAKE_SOURCE_DIR, so that a project which includes this module from elsewhere finds them too.
get_filename_component(CY_JOB_POOL_TOOLS "${CMAKE_CURRENT_LIST_DIR}/../tools/workflow" ABSOLUTE)

# Sets `out_var` to the program a compile is prefixed with and `<out_var>_LINK` to the one a link is,
# or both to "" when the pool is off.
function(cy_job_pool_command out_var)
    set(${out_var} "" PARENT_SCOPE)
    set(${out_var}_LINK "" PARENT_SCOPE)
    if(DEFINED ENV{CI} AND NOT DEFINED CACHE{CY_JOB_POOL})
        set(default OFF)
    else()
        set(default ON)
    endif()
    option(CY_JOB_POOL "Bound concurrent compiles and links machine-wide (tools/workflow/job_slot.py)"
           ${default})
    if(NOT CY_JOB_POOL)
        message(STATUS "Job pool: off (CY_JOB_POOL=OFF)")
        return()
    endif()
    if(WIN32 OR NOT CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
        message(STATUS "Job pool: off (${CMAKE_GENERATOR} on ${CMAKE_HOST_SYSTEM_NAME})")
        return()
    endif()
    # The system interpreter first: it starts in half the time of a distribution one, and this one
    # starts once per compile. Any python3 of 3.9 or later will do; the script is standard library.
    find_program(CY_JOB_POOL_PYTHON NAMES python3 HINTS /usr/bin
                 DOC "Interpreter for tools/workflow/job_slot.py")
    find_program(CY_JOB_POOL_BASH NAMES bash DOC "Shell for tools/workflow/jobs.sh")
    if(NOT CY_JOB_POOL_PYTHON OR NOT CY_JOB_POOL_BASH)
        message(STATUS "Job pool: off (python3 or bash not found)")
        return()
    endif()
    execute_process(
        COMMAND "${CY_JOB_POOL_BASH}" "${CY_JOB_POOL_TOOLS}/jobs.sh" machine
        OUTPUT_VARIABLE slots OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE status)
    if(NOT status EQUAL 0 OR NOT slots MATCHES "^[1-9][0-9]*$")
        message(STATUS "Job pool: off (tools/workflow/jobs.sh machine answered '${slots}')")
        return()
    endif()
    # ONE PATH, NO ARGUMENTS. ccache runs `prefix_command` by looking each space-separated word up as
    # a program, so `python3 -I -S job_slot.py --slots 22` fails every compile with "ccache: error:
    # -I: No such file or directory" — which is what the first version of this module did. The
    # options are therefore baked into two small scripts in the build tree, one for compiles and one
    # for links; -I -S keeps the interpreter's start short (no site-packages, no PYTHON* variables).
    #
    # A LINK IS `--link`: it gets NO jobserver (GCC 13.3's `lto1 -fwpa` deadlocks on one whose tokens
    # run out — cmake/profiles.cmake's cy_fix_lto_parallelism says how), and the launcher instead
    # waits for as many slots as the link's own `-flto=N` names before it starts, one for a link
    # without LTO. `--lto-jobs` is the count for a link that names none, the same CY_LTO_JOBS
    # profiles.cmake puts on the Shipping link line, so that a tree which configures the pool alone
    # bounds an `-flto=auto` link the same way.
    set(directory "${CMAKE_BINARY_DIR}/cy-launchers")
    set(script "${CY_JOB_POOL_TOOLS}/job_slot.py")
    set(run "exec '${CY_JOB_POOL_PYTHON}' -I -S '${script}' --slots ${slots}")
    set(link "${run} --link")
    if(DEFINED CY_LTO_JOBS AND CY_LTO_JOBS MATCHES "^[1-9][0-9]*$")
        string(APPEND link " --lto-jobs ${CY_LTO_JOBS}")
    endif()
    file(WRITE "${directory}/job-slot" "#!/bin/sh\n${run} -- \"$@\"\n")
    file(WRITE "${directory}/job-slot-link" "#!/bin/sh\n${link} -- \"$@\"\n")
    file(CHMOD "${directory}/job-slot" "${directory}/job-slot-link"
         PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                     WORLD_READ WORLD_EXECUTE)
    set(${out_var} "${directory}/job-slot" PARENT_SCOPE)
    set(${out_var}_LINK "${directory}/job-slot-link" PARENT_SCOPE)
    message(STATUS "Job pool: ${slots} slots machine-wide (tools/workflow/job_slot.py)")
endfunction()
