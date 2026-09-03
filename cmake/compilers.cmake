# cmake/compilers.cmake — the supported compilers, and the options every engine target compiles with.
#
# Task 1.2.5, and the flag half of task 1.2.1. `build-system-and-platforms` fixes the matrix:
# Clang 17+, GCC 13+, MSVC 19.38+ (Visual Studio 2022 17.8), each with the C++20 support the engine
# uses. A compiler outside it fails at configure time with a diagnostic naming what was found and
# what is required, rather than failing five minutes later inside a template.

set(CY_MINIMUM_Clang      17.0)
set(CY_MINIMUM_GNU        13.0)
set(CY_MINIMUM_MSVC       19.38)
# Apple's Clang carries its own version numbers. Xcode 15's AppleClang 15.0 is the first release
# whose C++20 support matches upstream Clang 17 for the features the engine uses.
set(CY_MINIMUM_AppleClang 15.0)

function(cy_check_compiler_support)
    set(id "${CMAKE_CXX_COMPILER_ID}")
    if(NOT DEFINED CY_MINIMUM_${id})
        message(FATAL_ERROR
            "Unsupported C++ compiler: ${id} ${CMAKE_CXX_COMPILER_VERSION}\n"
            "  at ${CMAKE_CXX_COMPILER}\n"
            "  CyberdyneEngine supports Clang ${CY_MINIMUM_Clang}+, GCC ${CY_MINIMUM_GNU}+, "
            "AppleClang ${CY_MINIMUM_AppleClang}+ and MSVC ${CY_MINIMUM_MSVC}+.\n"
            "  Run `just env-doctor` for the toolchain report.")
    endif()

    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS CY_MINIMUM_${id})
        message(FATAL_ERROR
            "${id} ${CMAKE_CXX_COMPILER_VERSION} is too old.\n"
            "  Found:    ${id} ${CMAKE_CXX_COMPILER_VERSION} at ${CMAKE_CXX_COMPILER}\n"
            "  Required: ${id} ${CY_MINIMUM_${id}} or later\n"
            "  Run `just env-doctor` for the toolchain report and the correction for this host.")
    endif()

    message(STATUS "C++ compiler: ${id} ${CMAKE_CXX_COMPILER_VERSION} "
                   "(minimum ${CY_MINIMUM_${id}})")
endfunction()

# Every engine target links cy::compile-options. It carries the language contract — C++20, no
# exceptions, no RTTI — and the warning set, as usage requirements rather than as directory-wide
# flags, so that third-party sources added by cmake/dependencies.cmake are not compiled under the
# engine's warning policy. `build-system-and-platforms` requires exactly that separation: a
# dependency's headers are included as system headers and do not fail the build.
function(cy_define_compile_options)
    add_library(cy_compile_options INTERFACE)
    add_library(cy::compile-options ALIAS cy_compile_options)

    target_compile_features(cy_compile_options INTERFACE cxx_std_20)

    if(MSVC)
        target_compile_options(cy_compile_options INTERFACE
            /permissive-        # conforming preprocessor and two-phase lookup
            /Zc:__cplusplus     # report the real __cplusplus value
            /Zc:preprocessor
            /utf-8
            /W4
            /wd4324             # structure padded due to alignment specifier: intended, and common
            $<$<BOOL:${CY_WARNINGS_AS_ERRORS}>:/WX>)
    else()
        target_compile_options(cy_compile_options INTERFACE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
            -Woverloaded-virtual
            -Wcast-align
            -Wdouble-promotion
            -Wformat=2
            -Wimplicit-fallthrough
            -Wnull-dereference
            -Wundef
            $<$<BOOL:${CY_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()

    # The diagnostics site name is built from __FILE__, and CMake hands the compiler absolute
    # paths, so without this every log record and every failed assertion in a trace or a crash
    # report carries the build machine's directory layout — including the account name it sits
    # under. `diagnostics-profiling-and-crash` classifies what an artefact may contain; a path
    # baked into a name rather than into a field is not reachable by the writer's redaction, so it
    # is removed here instead, at the only point that can. Debug information is deliberately left
    # alone: `just diagnose-crash --symbolicate` resolves addresses through it, on a machine that
    # has the sources.
    #
    # MSVC has no supported equivalent, so a Windows build still records absolute paths. That is
    # recorded rather than hidden; it is the one platform where the artefact is less clean.
    if(NOT MSVC)
        target_compile_options(cy_compile_options INTERFACE
            "-fmacro-prefix-map=${CMAKE_SOURCE_DIR}/=")
    endif()
endfunction()
