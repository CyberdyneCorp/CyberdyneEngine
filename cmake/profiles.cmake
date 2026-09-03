# cmake/profiles.cmake — the four build profiles, and what each one means in every toolchain.
#
# Task 0.1 / design.md §7. This is the milestone's named risk: four profile names that must mean the
# same thing in CMake, Cargo, the shader toolchain and the engine's own tools. Only CMake exists at
# M0, so the other columns are written down here, complete and unused, rather than being invented at
# M3 and M5 against whatever the CMake column happened to become.
#
# | Profile   | CMake config  | Assertions | Editor | Cargo (M5)                   | Slang (M3) |
# |-----------|---------------|------------|--------|------------------------------|------------|
# | `debug`   | `Debug`       | on         | on     | `dev`                        | `-O0 -g`   |
# | `dev`     | `Development` | on         | on     | `dev` + opt-level 2          | `-O1 -g`   |
# | `profile` | `Profile`     | off        | off    | `release` + debug            | `-O2 -g`   |
# | `release` | `Shipping`    | off        | off    | `release` + LTO              | `-O3`      |
#
# The CMake column is authoritative *here and nowhere else*. `just` passes the profile name and this
# file derives the configuration; when a caller supplies both, they are cross-checked, so the table
# in the `justfile` cannot drift away from this one without a configure error.

set(CY_PROFILES debug dev profile release
    CACHE INTERNAL "The named build profiles, in increasing order of optimisation")

# --- The CMake column ---------------------------------------------------------------------------

set(CY_PROFILE_debug_CONFIG   Debug)
set(CY_PROFILE_dev_CONFIG     Development)
set(CY_PROFILE_profile_CONFIG Profile)
set(CY_PROFILE_release_CONFIG Shipping)

set(CY_BUILD_CONFIGURATIONS Debug Development Profile Shipping
    CACHE INTERNAL "The build configurations, one per profile")

# Configurations in which CY_DEVELOPMENT is defined: assertions, diagnostics, hot reload and debug
# visualisation are gated on it, and it is absent from Profile and Shipping binaries.
set(CY_DEVELOPMENT_CONFIGURATIONS Debug Development
    CACHE INTERNAL "Configurations that define CY_DEVELOPMENT")

# --- The reserved columns -----------------------------------------------------------------------
#
# Neither toolchain is present at M0. These are recorded so that adding them is a lookup rather than
# a decision, and so that the claim "the profiles survive contact with Cargo and Slang" is checkable
# rather than hopeful.
#
# Cargo: four named workspace profiles, so each engine profile selects exactly one and Cargo's own
# `target/<profile>/` keeps their artefacts apart. `dev` and `release` are Cargo's built-ins; the
# other two are custom profiles declared with `inherits`, stable since Rust 1.57.
#
#   [profile.dev]          opt-level = 0, debug = true,  debug-assertions = true
#   [profile.development]  inherits = "dev",     opt-level = 2
#   [profile.profiling]    inherits = "release", debug = true, debug-assertions = false
#   [profile.shipping]     inherits = "release", lto = "fat", codegen-units = 1, strip = "debuginfo"

set(CY_PROFILE_debug_CARGO   dev)
set(CY_PROFILE_dev_CARGO     development)
set(CY_PROFILE_profile_CARGO profiling)
set(CY_PROFILE_release_CARGO shipping)

# Slang: `slangc` optimisation and debug-information levels, passed verbatim.
set(CY_PROFILE_debug_SLANG   "-O0 -g")
set(CY_PROFILE_dev_SLANG     "-O1 -g")
set(CY_PROFILE_profile_SLANG "-O2 -g")
set(CY_PROFILE_release_SLANG "-O3")

# --- Phase one: called before project() ----------------------------------------------------------
#
# A macro, not a function: it sets CMAKE_BUILD_TYPE and CMAKE_CONFIGURATION_TYPES in the caller's
# scope, and both must be established before project() so that the generator sees them.

macro(cy_declare_build_configurations)
    set(CMAKE_CONFIGURATION_TYPES "${CY_BUILD_CONFIGURATIONS}" CACHE STRING
        "The build configurations available to multi-configuration generators" FORCE)

    if(DEFINED CY_PROFILE)
        if(NOT "${CY_PROFILE}" IN_LIST CY_PROFILES)
            message(FATAL_ERROR
                "CY_PROFILE is '${CY_PROFILE}', which is not a build profile.\n"
                "  Profiles: ${CY_PROFILES}\n"
                "  Run `just` to see the recipes that select one.")
        endif()
        cy_check_profile_agrees_with_configuration()
        set(CMAKE_BUILD_TYPE "${CY_PROFILE_${CY_PROFILE}_CONFIG}" CACHE STRING
            "The build configuration, derived from CY_PROFILE=${CY_PROFILE}" FORCE)
    elseif(NOT CMAKE_BUILD_TYPE)
        # Development is the day-to-day configuration, so it is what an unqualified build produces.
        # A multi-configuration generator ignores this; it chooses per build invocation.
        set(CMAKE_BUILD_TYPE "Development" CACHE STRING "The build configuration" FORCE)
    endif()

    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS ${CY_BUILD_CONFIGURATIONS})

    if(CMAKE_BUILD_TYPE AND NOT "${CMAKE_BUILD_TYPE}" IN_LIST CY_BUILD_CONFIGURATIONS)
        message(FATAL_ERROR
            "CMAKE_BUILD_TYPE is '${CMAKE_BUILD_TYPE}', which is not a build configuration.\n"
            "  Configurations: ${CY_BUILD_CONFIGURATIONS}")
    endif()

    # Imported targets from dependencies rarely define the engine's own configurations; map each one
    # onto the nearest configuration a dependency is likely to have built.
    set(CMAKE_MAP_IMPORTED_CONFIG_DEVELOPMENT Development Release RelWithDebInfo "")
    set(CMAKE_MAP_IMPORTED_CONFIG_PROFILE     Profile     Release RelWithDebInfo "")
    set(CMAKE_MAP_IMPORTED_CONFIG_SHIPPING    Shipping    Release RelWithDebInfo "")
endmacro()

# A profile and an explicit configuration may both be supplied — `just` supplies both precisely so
# that its own copy of the table is checked against this one. They must agree.
macro(cy_check_profile_agrees_with_configuration)
    if(CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE STREQUAL "${CY_PROFILE_${CY_PROFILE}_CONFIG}")
        message(FATAL_ERROR
            "Profile '${CY_PROFILE}' maps to configuration '${CY_PROFILE_${CY_PROFILE}_CONFIG}', "
            "but CMAKE_BUILD_TYPE was set to '${CMAKE_BUILD_TYPE}'.\n"
            "  One of the two tables is stale: cmake/profiles.cmake, or the profile table in the "
            "justfile.")
    endif()
endmacro()

# --- Phase two: called after project() ------------------------------------------------------------
#
# The compiler is known only once project() has run, so the per-configuration flags are set here.
# They are set as the ordinary CMAKE_<LANG>_FLAGS_<CONFIG> cache entries rather than as directory
# properties, because that is where every generator, every dependency added by FetchContent, and
# anyone reading `cmake -LA` expects to find them. They are FORCEd: the table above is the definition
# of what a configuration means, and a stale cache entry silently redefining one is the failure this
# whole file exists to prevent. Per-invocation additions go in CMAKE_<LANG>_FLAGS, not here.

function(cy_apply_build_configurations)
    if(MSVC)
        cy_set_configuration_flags(Debug       "/Od /Zi /DCY_DEVELOPMENT" "/DEBUG /INCREMENTAL:NO")
        cy_set_configuration_flags(Development "/O2 /Zi /DCY_DEVELOPMENT" "/DEBUG /INCREMENTAL:NO")
        cy_set_configuration_flags(Profile     "/O2 /Zi /DNDEBUG"         "/DEBUG /INCREMENTAL:NO")
        cy_set_configuration_flags(Shipping    "/O2 /DNDEBUG"             "")
        # Select the runtime library explicitly: the engine's configurations are not the ones CMake's
        # default expression knows about, so Development, Profile and Shipping would otherwise pick
        # the debug runtime by omission.
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" PARENT_SCOPE)
    else()
        cy_set_configuration_flags(Debug       "-O0 -g -DCY_DEVELOPMENT" "")
        cy_set_configuration_flags(Development "-O2 -g -DCY_DEVELOPMENT" "")
        cy_set_configuration_flags(Profile     "-O2 -g -DNDEBUG"         "")
        cy_set_configuration_flags(Shipping    "-O3 -DNDEBUG"            "")
    endif()

    # Shipping is the only configuration that pays for link-time optimisation. Symbols are stripped
    # to a separate artefact at packaging time, not here; that is `build-and-packaging`.
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_reason)
    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_SHIPPING TRUE PARENT_SCOPE)
    else()
        message(STATUS "Shipping link-time optimisation unavailable: ${ipo_reason}")
    endif()
endfunction()

function(cy_set_configuration_flags config compile_flags link_flags)
    string(TOUPPER "${config}" upper)
    foreach(lang C CXX)
        set(CMAKE_${lang}_FLAGS_${upper} "${compile_flags}" CACHE STRING
            "${lang} flags for the ${config} configuration" FORCE)
    endforeach()
    foreach(kind EXE SHARED MODULE)
        set(CMAKE_${kind}_LINKER_FLAGS_${upper} "${link_flags}" CACHE STRING
            "${kind} linker flags for the ${config} configuration" FORCE)
    endforeach()
endfunction()

# The profile a configuration came from, for anything that has only the configuration in hand.
function(cy_profile_for_configuration config out_var)
    foreach(profile IN LISTS CY_PROFILES)
        if(CY_PROFILE_${profile}_CONFIG STREQUAL config)
            set(${out_var} "${profile}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()
