# tools/material/ — layer 7: material cooking, as a node in the derivation graph. M7 task 6.3.
#
# `material-compiler` at Working. The COMPILER is engine code and lives in src/rendering/material/ —
# the specification requires it ("The material compiler SHALL be engine code") and the reason is
# that a runtime creates material instances and previews nodes without a tool present. What is here
# is the COOK: the producer that turns an authored `.cymat` into cooked programs, and the front end
# that runs it.
#
# TWO TARGETS, FOR THE REASON tools/cook/, tools/import/ AND tools/build/ EACH HAVE TWO.
# `cy_material_cook` is a library with no `main`: the interesting assertions — that two runs produce
# identical bytes, that the second run hits the cache, that a broken material produces no artefact —
# are on the bundle and on the build report, not on an exit code. `cy_material` is the thin front
# end.
#
# NO SECOND CACHE. M6's gate recorded "there are still two derivation-key functions and two
# derived-data caches"; M7 task 1.1 merged them, and the whole point of putting this cook in the
# graph is that it uses that one — `cy::build`'s derivation key, its content-addressed artefact
# store, its precise invalidation and its two-run determinism gate. Nothing here caches anything.
#
# LAYER tools, which may link any layer: a tool consumes the engine rather than sitting in it.
cy_add_module(
    NAME cy_material_cook
    LAYER tools
    ALIAS cy::material-cook
    TYPE STATIC
    SOURCES
        src/cook.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::build-graph cy::rendering-material)

cy_add_module(
    NAME cy_material
    LAYER tools
    TYPE EXECUTABLE
    SOURCES
        src/main.cpp
    PRIVATE_DEPENDENCIES cy::material-cook)

# tests/ is read now and declares its suites later; see the comment at the top of it for why.
if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
