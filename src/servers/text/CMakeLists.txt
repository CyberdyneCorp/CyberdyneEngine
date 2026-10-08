# src/servers/text/ — layer 2: CyberText. M5 task 5.3.
#
# `text-and-fonts` reached **Seed** here at M5: the interface, the atlas and the minimal backend.
# M11.e (issue #86) added the seam the outline-font libraries sit beneath — `TextBackend` in
# include/cy/servers/text/backend.h — the cooked-font format the importer writes, a page per pixel
# format, and bidirectional line layout.
#
# WHY THERE IS STILL NO HARFBUZZ, ICU OR FREETYPE HERE. They live in src/backends/text-complete/ at
# layer 3, which is the same place SDL, Jolt and miniaudio live and for the same reason: no
# third-party type may appear above the backend, and tools/layercheck/layercheck.py's `thirdparty`
# check fails the build if one of their headers is included from here.
#
# NOTHING HERE IS BEHIND A CY_* OPTION. The interface and the minimal backend compile in every
# build, so the suites that exercise them run in every configuration rather than in the ones somebody
# remembered to turn an option on for. That is the shape src/servers/physics/ chose for the same
# reason, and its CMakeLists says so at greater length.
#
# THE DEPENDENCIES, AND WHY EACH IS WHERE IT IS.
#   cy::core-base    public   Expected, Error, the fixed-width types. Every header uses them.
#   cy::core-math    public   Vec2, IRect and geom::AtlasPacker, which the glyph atlas packs with.
#   cy::core-memory  public   Array and Span appear in public headers.
#   cy::core-values  public   cy::Handle is the values module's type; a face is one.
#   cy::text-unicode public   BidiResult appears in backend.h; see the note at the link below.
#
# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_servers_text
    LAYER servers
    ALIAS cy::servers-text
    TYPE STATIC
    SOURCES
        src/atlas.cpp
        src/cooked_font.cpp
        src/layout.cpp
        src/server.cpp
        src/text.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::core-values
        # src/text/: the bidirectional algorithm the server falls back to when a backend has none
        # of its own, the grapheme property that keeps a mark in its base's face, and the UTF-8
        # decoder both modules once defined separately. Same layer, and it depends on core alone.
        cy::text-unicode)

# tests/ is read now and declares its suites later; see the comment at the top of it for why.
if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
