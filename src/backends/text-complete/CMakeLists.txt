# src/backends/text-complete/ — layer 3: FreeType, HarfBuzz, msdfgen and ICU behind
# `cy::text::TextBackend`. M11.e, issue #86.
#
# `text-and-fonts`: "no HarfBuzz, ICU, or FreeType type SHALL appear outside the backend". Three
# things hold it:
#   1. the interface is `cy::text::TextBackend` at layer 2, which cannot include a layer-3 header —
#      tools/layercheck/layercheck.py's `includes` check fails the build over it;
#   2. every library is linked PRIVATE below, so no consumer inherits its include directories and
#      `#include <hb.h>` does not even resolve outside this target;
#   3. the `thirdparty` check names each library's headers and fails the build if one is included
#      from anywhere but this directory — and inside it, each is named by one translation unit
#      (src/internal.h says which).
#
# GATED BY CY_TEXT (cmake/features.cmake), which also gates the FreeType, HarfBuzz and msdfgen
# fetch. ICU is gated separately by CY_TEXT_ICU: with it off, src/icu_bidi.cpp compiles to a stub
# and the text server resolves bidirectional levels with src/text/'s algorithm instead.
#
# Sources are listed explicitly and never globbed.
set(cy_text_complete_private cy::dep::harfbuzz cy::dep::freetype cy::dep::msdfgen)
if(CY_TEXT_ICU)
    list(APPEND cy_text_complete_private cy::dep::icu)
endif()

cy_add_module(
    NAME cy_text_complete
    LAYER backends
    ALIAS cy::text-complete
    TYPE STATIC
    SOURCES
        src/complete_backend.cpp
        src/freetype_faces.cpp
        src/harfbuzz_shaper.cpp
        src/icu_bidi.cpp
        src/msdf_generator.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PUBLIC_DEPENDENCIES cy::servers-text
    PRIVATE_DEPENDENCIES ${cy_text_complete_private})

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
