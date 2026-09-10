# src/text/ — the Unicode layer above CyberText. M8.b task 9.4.
#
# WHAT IS HERE, AND WHY IT IS NOT src/servers/text/. `text-and-fonts` reached Seed at M4 in
# src/servers/text/ at layer 2: the interface, the glyph atlas, the minimal image-grid backend, and
# layout objects that declare — through `TextCapabilities` — that they cannot shape, cannot do
# bidirectional layout and cannot break Thai. This module is those three algorithms, and the
# localisation the specification's last requirement asks for.
#
# IT DEPENDS ON NOTHING BUT cy::core. That is deliberate: the algorithms are functions over text and
# property tables, and a module that needed a font server to answer "which way does this run go"
# could not be used by a cook, a test or a tool. `cy::servers-text` is the CONSUMER of this module
# rather than its dependency, and `src/ui/` is the consumer of both.
#
# HARFBUZZ, ICU AND FREETYPE ARE STILL NOT IN deps/manifest.toml. Every header here states what it
# implements and what it does not, and `ShapingCapabilities` answers it in code. See README.md for
# the honest tier this module reaches and the one it does not.
#
# LAYER. `servers` (2): it is a peer of src/servers/text/ rather than something above it, because it
# names nothing that a server may not name — no scene, no world, no device, no allocator policy
# beyond the one it is handed.
cy_add_module(
    NAME cy_text
    LAYER servers
    ALIAS cy::text-unicode
    TYPE STATIC
    SOURCES
        src/bidi.cpp
        src/linebreak.cpp
        src/localisation.cpp
        src/shaping.cpp
        src/unicode.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::core-values)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
