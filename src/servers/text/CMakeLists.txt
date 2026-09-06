# src/servers/text/ — layer 2: CyberText. M5 task 5.3.
#
# `text-and-fonts` reaches **Seed** at M5: "interfaces, data model and invariants exist; dependents
# can be built against it". What is real and what is deliberately absent is written out in
# include/cy/servers/text/text.h; the short version is that the interface is complete and the only
# backend behind it is the minimal one the specification itself carves out.
#
# WHY THERE IS NO HARFBUZZ, ICU OR FREETYPE HERE. None of the three is in deps/manifest.toml at M5,
# and that file's header says why. The seam is the shape of this module — an engine-owned interface
# over a replaceable backend — and the day they are integrated they live in src/backends/text-*/ at
# layer 3, which is the same place SDL, Jolt and miniaudio live and for the same reason: no
# third-party type may appear above the backend, and the layer checker is what enforces it.
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
        src/layout.cpp
        src/server.cpp
        src/text.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::core-values)

# tests/ is read now and declares its suites later; see the comment at the top of it for why.
if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
