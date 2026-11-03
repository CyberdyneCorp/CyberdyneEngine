# tools/deps/test_fetch_depth.cmake — which pins cmake/dependencies.cmake may clone shallow.
#
#     cmake -P tools/deps/test_fetch_depth.cmake
#
# Run by `just maintenance-deps-check`. A pin a shallow clone cannot reach is a configure failure
# on every fresh tree the day its branch moves, and nothing before that day shows it, so the
# decision is checked here against the manifest's spellings rather than left to the next clone.

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/dependency_fetch.cmake")

set(_failures 0)
function(expect repository tag expected)
    cy_dependency_fetch_shallow("${repository}" "${tag}" _got)
    if(NOT _got STREQUAL expected)
        message(WARNING "${repository} at `${tag}`: shallow=${_got}, expected ${expected}")
        math(EXPR _n "${_failures} + 1")
        set(_failures ${_n} PARENT_SCOPE)
    endif()
endfunction()

# A release tag on GitHub: the shallow clone fetches the tagged commit.
expect("https://github.com/libsdl-org/SDL" "release-3.4.16" TRUE)
expect("https://github.com/facebook/zstd" "v1.5.7" TRUE)
expect("https://github.com/harfbuzz/harfbuzz" "14.6.0" TRUE)
# A branch pin: a tip today, unreachable once upstream pushes (stb_vorbis, November 2026).
expect("https://github.com/nothings/stb" "master@003501f" FALSE)
expect("https://github.com/jpcy/xatlas" "master@f700c77" FALSE)
# A commit no release names.
expect("https://github.com/microsoft/PixEvents" "unreleased-b0caa735" FALSE)
# A host that refuses shallow fetches of a commit, whatever the pin looks like.
expect("https://gitlab.com/libeigen/eigen" "3.4-e7248b2" FALSE)
expect("https://gitlab.com/libeigen/eigen" "3.4.0" FALSE)

if(_failures GREATER 0)
    message(FATAL_ERROR "fetch-depth: ${_failures} case(s) failed")
endif()
message(STATUS "fetch-depth: every case passed")
