# tools/ship/ — distributable game builds: the signed manifest, its startup check, and the packager.
# Issue #87, the packaging half of M11.e's "Ship".
#
#   cy::ship     the library: Ed25519 signatures (signature.h), the manifest (manifest.h), the
#                startup verification and content materialisation (installation.h), the packer
#                (pack.h), the settings file (settings.h), and a game's launch (launch.h).
#   cy_ship      the command line over it: pack, verify, read, keygen, public-key, trusted.
#   package.py   `just package <game> <target>`: stages a game for a desktop target, packs it with
#                cy_ship, and archives it — a tarball, a zip, or an ad-hoc signed macOS bundle.
#
# A TOOLS-LAYER LIBRARY LINKED INTO GAMES. The games this packages — samples/13-rts-api,
# samples/14-third-person, samples/15-first-person and samples/11-ship — are tools-layer programs
# themselves, so the check they start with lives beside the packer that writes what it checks.
# README.md says when that should move to src/runtime/.
#
# The key a build trusts is configured here, once, and compiled into cy::ship:
#
#   -D CY_SHIP_PUBLIC_KEY=<64 hex digits>   trust this key (`cy_ship keygen` prints one); content
#                                           the development key signed is then refused
#   -D CY_SHIP_REQUIRE_INSTALLATION=ON      a game refuses to start outside a verified installation

set(CY_SHIP_PUBLIC_KEY "" CACHE STRING
    "The Ed25519 public key (64 hex digits) packaged games trust; empty trusts the development key")
option(CY_SHIP_REQUIRE_INSTALLATION
       "Packaged games refuse to start anywhere but a verified installation" OFF)

if(CY_SHIP_PUBLIC_KEY AND NOT CY_SHIP_PUBLIC_KEY MATCHES "^[0-9a-fA-F]+$")
    message(FATAL_ERROR "CY_SHIP_PUBLIC_KEY is not hex: '${CY_SHIP_PUBLIC_KEY}'")
endif()
string(LENGTH "${CY_SHIP_PUBLIC_KEY}" cy_ship_key_length)
if(CY_SHIP_PUBLIC_KEY AND NOT cy_ship_key_length EQUAL 64)
    message(FATAL_ERROR "CY_SHIP_PUBLIC_KEY is ${cy_ship_key_length} hex digits; an Ed25519 "
                        "public key is 64")
endif()

cy_add_module(
    NAME cy_ship
    LAYER tools
    ALIAS cy::ship
    TYPE STATIC
    SOURCES
        src/signature.cpp
        src/manifest.cpp
        src/installation.cpp
        src/pack.cpp
        src/settings.cpp
        src/launch.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-assets
    PRIVATE_DEPENDENCIES cy::core-diagnostics cy::platform-desktop-sdl3 cy::dep::monocypher)
if(CY_SHIP_PUBLIC_KEY)
    target_compile_definitions(cy_ship PRIVATE "CY_SHIP_PUBLIC_KEY=\"${CY_SHIP_PUBLIC_KEY}\"")
endif()
if(CY_SHIP_REQUIRE_INSTALLATION)
    target_compile_definitions(cy_ship PRIVATE CY_SHIP_REQUIRE_INSTALLATION=1)
endif()

cy_add_module(
    NAME cy_ship_tool
    LAYER tools
    TYPE EXECUTABLE
    SOURCES src/main.cpp
    PRIVATE_DEPENDENCIES cy::ship)
set_target_properties(cy_ship_tool PROPERTIES OUTPUT_NAME cy_ship)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
