# cy_declare_determinism_profile() — the configuration-time half of M9 task 2.2.
#
# ==================================================================================================
# WHY A CMAKE FUNCTION IS PART OF A DETERMINISM PROFILE
# ==================================================================================================
#
# `simulation-and-determinism`'s exit criterion is the strong one: "A session declaring a
# determinism profile a subsystem cannot meet is **rejected at configuration**, not discovered
# later." M9's spike found that the profile is not a property of source alone.
# `openspec/changes/implement-m9-integrity/design.md` §1.2, measured:
#
#   * Two builds of identical source, differing only in `-march` and a floating-point contraction
#     flag, produce different state hashes. 13 of 16 workloads move; clang and gcc disagree with
#     each other in 8 of them; 231 of 267 values move in the `state_hash` workload itself.
#   * They agree today only because the engine targets baseline x86-64, which has no fused
#     multiply-add for the compiler's default contraction setting to use.
#     `src/core/math/tests/CMakeLists.txt` already anticipates `-march=x86-64-v3` in as many words,
#     so the day the baseline moves is a day this repository has already written down.
#
# So a refusal that only looked at which subsystems were linked would pass a build whose flags make
# the profile unachievable. This function is the earliest moment that refusal can happen: it fails
# the CMake configure, naming the module and the flag.
#
# ==================================================================================================
# WHAT IT DOES *NOT* DO, AND WHY THAT MATTERS
# ==================================================================================================
#
# **It does not add `-ffp-contract=off` for you.** It requires the module to have added it, and
# fails if it has not. A function that quietly fixed the flag would be a check that can never fail,
# which is the defect class this project's last thirteen gates found eighteen times. Deleting the
# `target_compile_options()` line from a covered module is therefore a one-line mutation that turns
# the configure red, and `src/core/determinism/README.md` records that it was run.
#
# THE COVERAGE IS SMALL AND THAT IS STATED RATHER THAN HIDDEN. Three engine modules declare a
# profile today — `cy_core_determinism`, `cy_gameplay` and `cy_replay` — out of the engine's
# eighty-one, plus the two suites that assert what the profile's own build half reports. A fourth
# module, `cy_core_math`, carries the flag already and does not declare, because M9's command-log
# phase does not own `src/core/math/CMakeLists.txt`.
#
# The manifest this function writes is what makes the gap countable instead of rhetorical:
# `determinism_lint.py --report-coverage` reads it and prints the ratio on every run. It counts
# **built targets** rather than modules, so its denominator is the larger number — the tests and the
# tools are targets too — and the ratio moves as the tree grows, which is the point of taking it as
# a reading rather than maintaining it as a figure.

# The compile option that turns contraction off, per compiler. MSVC's `/fp:precise` is its nearest
# equivalent and is NOT EVALUATED here: this project builds on one operating system with two
# compilers, and a claim about a third would be a claim nothing checked.
function(_cy_determinism_contraction_flag out)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set(${out} "-ffp-contract=off" PARENT_SCOPE)
    elseif(MSVC)
        set(${out} "/fp:precise" PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()

# Declare that `target` is compiled to meet `PROFILE`.
#
#   cy_declare_determinism_profile(<target> PROFILE <name> REASON <text>)
#
# `REASON` is required and is quoted in the failure, because "this module declares SamePlatform" is
# not by itself an argument that it can.
function(cy_declare_determinism_profile target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "PROFILE;REASON" "")

    if(NOT arg_PROFILE)
        message(FATAL_ERROR "cy_declare_determinism_profile(${target}): PROFILE is required.")
    endif()
    if(NOT arg_REASON)
        message(FATAL_ERROR
            "cy_declare_determinism_profile(${target}): REASON is required. A declaration with no "
            "argument behind it is the shape of a claim nothing checked.")
    endif()
    # The five names, inside the function rather than beside it. A `set()` at file scope would be
    # visible only in the directory that included this file — which is how the first version of this
    # function reported that 'SamePlatform' is not one of '' when src/gameplay/ called it.
    set(known None ReplayStable SamePlatform CrossPlatform Lockstep)
    list(FIND known "${arg_PROFILE}" position)
    if(position LESS 0)
        message(FATAL_ERROR
            "cy_declare_determinism_profile(${target}): '${arg_PROFILE}' is not one of ${known}.")
    endif()
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "cy_declare_determinism_profile(${target}): no such target. Declare the profile after "
            "cy_add_module().")
    endif()

    # The translation units of a covered module learn what they were compiled with, so that
    # `BuildConfiguration::from_build()` reports a measured fact rather than a hopeful default. It
    # is PRIVATE: a module that never declared a profile must report the truth — that nothing
    # checked its flags — rather than inheriting a neighbour's answer through a PUBLIC define.
    target_compile_definitions(${target} PRIVATE CY_DETERMINISM_CONTRACTION_OFF=1)

    set_property(GLOBAL APPEND PROPERTY CY_DETERMINISM_PROFILE_TARGETS "${target}")
    set_property(GLOBAL PROPERTY CY_DETERMINISM_PROFILE_OF_${target} "${arg_PROFILE}")
    set_property(GLOBAL PROPERTY CY_DETERMINISM_REASON_OF_${target} "${arg_REASON}")

    get_property(deferred GLOBAL PROPERTY CY_DETERMINISM_VERIFY_DEFERRED)
    if(NOT deferred)
        set_property(GLOBAL PROPERTY CY_DETERMINISM_VERIFY_DEFERRED TRUE)
        # Deferred to the end of the top-level directory's processing so that compile options added
        # after this call — by the module itself, by a later `target_compile_options()` — are seen.
        # Checking here would check a half-built target and pass things it should refuse.
        #
        # RE-SCHEDULABLE ON PURPOSE. Test suites are themselves declared from a deferred call (a
        # suite has to read the taxonomy back out of tests/, which is processed after src/), so
        # declarations arrive *during* deferral processing. `cy_determinism_verify_profiles()`
        # clears this flag as its first act, so a declaration made after it ran schedules another
        # pass rather than being silently unchecked — which is how the manifest ends up naming
        # every covered target instead of the ones that happened to be early.
        cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_determinism_verify_profiles)
    endif()
endfunction()

# The refusal. Runs once, at the end of configuration, over every declared target.
function(cy_determinism_verify_profiles)
    # Cleared first: see the note in cy_declare_determinism_profile(). A declaration arriving after
    # this pass re-schedules one.
    set_property(GLOBAL PROPERTY CY_DETERMINISM_VERIFY_DEFERRED FALSE)
    get_property(targets GLOBAL PROPERTY CY_DETERMINISM_PROFILE_TARGETS)
    if(NOT targets)
        return()
    endif()
    list(REMOVE_DUPLICATES targets)
    list(SORT targets)

    _cy_determinism_contraction_flag(contraction)
    set(manifest "# module profile — written by cy_declare_determinism_profile()\n")

    foreach(target IN LISTS targets)
        get_property(profile GLOBAL PROPERTY CY_DETERMINISM_PROFILE_OF_${target})
        get_property(reason GLOBAL PROPERTY CY_DETERMINISM_REASON_OF_${target})
        get_target_property(options ${target} COMPILE_OPTIONS)
        if(NOT options)
            set(options "")
        endif()

        # Fast-math is disallowed on an authoritative path under every deterministic profile, and it
        # is checked first: it would make every module fail, and naming one of them would send the
        # reader to the wrong file.
        foreach(option IN LISTS options)
            if(option MATCHES "fast-math|/fp:fast|-Ofast")
                message(FATAL_ERROR
                    "Determinism profile refused at configuration.\n"
                    "  Module    : ${target}\n"
                    "  Declares  : ${profile} — ${reason}\n"
                    "  Guarantee : fast-math transformations disallowed on authoritative paths\n"
                    "  Found     : ${option}\n"
                    "  simulation-and-determinism, 'Floating-point policy': a controlled "
                    "floating-point environment, with fast-math transformations that alter results "
                    "disallowed on authoritative paths.")
            endif()
        endforeach()

        if(NOT profile STREQUAL "None" AND NOT profile STREQUAL "ReplayStable")
            if(contraction STREQUAL "")
                message(FATAL_ERROR
                    "Determinism profile refused at configuration.\n"
                    "  Module    : ${target}\n"
                    "  Declares  : ${profile} — ${reason}\n"
                    "  Guarantee : floating-point contraction off\n"
                    "  Found     : compiler '${CMAKE_CXX_COMPILER_ID}' has no known flag for it, "
                    "so the guarantee is NOT EVALUATED and may not be claimed.")
            endif()
            list(FIND options "${contraction}" found)
            if(found LESS 0)
                message(FATAL_ERROR
                    "Determinism profile refused at configuration.\n"
                    "  Module    : ${target}\n"
                    "  Declares  : ${profile} — ${reason}\n"
                    "  Guarantee : floating-point contraction off\n"
                    "  Missing   : ${contraction} on this target's COMPILE_OPTIONS\n"
                    "  M9's spike measured two builds of identical source, differing only in "
                    "-march and this flag, producing different state hashes: 231 of 267 values "
                    "moved in the state_hash workload. See "
                    "openspec/changes/implement-m9-integrity/design.md section 1.2.")
            endif()
        endif()

        string(APPEND manifest "${target} ${profile}\n")
    endforeach()

    # The manifest the determinism lint reads. Written rather than passed, because the lint runs as
    # a test in a build directory and a list threaded through five CMake scopes is a list that
    # drifts.
    file(WRITE "${CMAKE_BINARY_DIR}/determinism-profiles.txt" "${manifest}")
endfunction()
