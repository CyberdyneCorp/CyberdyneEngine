# tools/roadmap/probes/profiles/ — ASK CMAKE WHAT A PROFILE MEANS, rather than read what it says.
#
# `m11a:build-system-at-working` re-checks `build-system-and-platforms`'s first leg — FOUR PROFILES
# THAT MEAN THE SAME THING IN EVERY TOOLCHAIN — and until M11's second repair round it did so with
# regular expressions over `cmake/profiles.cmake`'s text. The repair round demonstrated what that is
# worth: comment out every `set(CY_PROFILES ...)` and `set(CY_PROFILE_<p>_<column> ...)` line, so
# that the file defines nothing and the build has no profile table at all, and the criterion stayed
# GREEN — the regular expressions matched the commented-out lines. The same defect as the taxonomy
# probe beside this one, in a second file.
#
# So the profile table is EVALUATED here. `cy_declare_build_configurations()` is the macro the
# top-level CMakeLists.txt calls, called the same way and before `project()` as it requires, so
# `CMAKE_BUILD_TYPE` in the report is the configuration the BUILD would use for `CY_PROFILE=<p>`,
# derived by the shipped code. `cy_profile_for_configuration()` maps it back, and
# `cy_declare_features()` declares the feature option set through its own resolver, so a feature row
# that exists only in a comment declares no option and is absent from the report.
#
# One configure per profile, LANGUAGES NONE, about a tenth of a second each: this runs on every pull
# request through `milestone-m11a` and inside `falsify.prove`'s sandboxed copy of the tracked tree,
# which has no build and no compiler.
#
# Driven by `tools/roadmap/row_evidence.py`. Three variables come in on the command line:
#   CY_REPO          the repository root to read cmake/ from
#   CY_PROFILE       the profile to evaluate — the same variable `just` passes the real build
#   CY_PROBE_REPORT  the file the report is written to

cmake_minimum_required(VERSION 3.24)

foreach(required CY_REPO CY_PROFILE CY_PROBE_REPORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "the profiles probe needs -D${required}=<value>")
    endif()
endforeach()

include("${CY_REPO}/cmake/profiles.cmake")

# Before project(), which is the contract the macro's own comment states: it sets CMAKE_BUILD_TYPE
# and CMAKE_CONFIGURATION_TYPES and both must be established before the generator sees them. A
# profile the table does not carry is a FATAL_ERROR here, raised by the shipped macro.
cy_declare_build_configurations()

project(cy_profiles_probe LANGUAGES NONE)

cy_profile_for_configuration("${CMAKE_BUILD_TYPE}" _profile_again)

include("${CY_REPO}/cmake/features.cmake")
cy_declare_features()
cy_feature_names(_features)

# Every list is joined with a space before it is written: a CMake list is semicolon separated and
# `file(WRITE)` takes its arguments as a list, so writing one unjoined silently concatenates its
# elements — "debugdevprofilerelease" — and the reader would have compared against that.
string(JOIN " " _profiles ${CY_PROFILES})
string(JOIN " " _configurations ${CY_BUILD_CONFIGURATIONS})
string(JOIN " " _development ${CY_DEVELOPMENT_CONFIGURATIONS})

file(WRITE  "${CY_PROBE_REPORT}" "profiles|${_profiles}\n")
file(APPEND "${CY_PROBE_REPORT}" "configurations|${_configurations}\n")
file(APPEND "${CY_PROBE_REPORT}" "development|${_development}\n")
set(_cargo "${CY_PROFILE_${CY_PROFILE}_CARGO}")
set(_slang "${CY_PROFILE_${CY_PROFILE}_SLANG}")
file(APPEND "${CY_PROBE_REPORT}"
     "selected|${CY_PROFILE}|${CMAKE_BUILD_TYPE}|${_cargo}|${_slang}\n")
file(APPEND "${CY_PROBE_REPORT}" "roundtrip|${CMAKE_BUILD_TYPE}|${_profile_again}\n")
foreach(_feature IN LISTS _features)
    file(APPEND "${CY_PROBE_REPORT}" "feature|${_feature}|${${_feature}}\n")
endforeach()
