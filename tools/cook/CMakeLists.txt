# tools/cook/ — layer 7: the cook path. M2 task 3.2.13.
#
# `core-assets-and-io` requires that "the runtime SHALL load only cooked assets". M1 built the
# package format's write path and left `PackageWriter::add` taking bytes that were already cooked;
# `src/scene/serialization/` produces those bytes. This is the driver that joins them, and it is the
# first thing in the engine that turns a directory of authored files into a package.
#
# TWO TARGETS, AND THE SPLIT IS THE REASON THE PIPELINE IS TESTABLE. `cy_cook_pipeline` is a library
# with no `main` and no argument parsing; `cy_cook` is a thin front end over it. A cook that existed
# only as an executable would be testable only by running a process and reading its exit code, and
# the interesting assertions — how many relationships flattened, which references were nulled — are
# on the report rather than on the exit code.
#
# LAYER tools, which may link any layer: a tool consumes the engine rather than sitting in it.

cy_add_module(
    NAME cy_cook_pipeline
    LAYER tools
    ALIAS cy::cook-pipeline
    TYPE STATIC
    SOURCES
        src/pipeline.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-assets cy::scene-serialization cy::ecs)

cy_add_module(
    NAME cy_cook
    LAYER tools
    TYPE EXECUTABLE
    SOURCES
        src/main.cpp
    PRIVATE_DEPENDENCIES cy::cook-pipeline)

# tests/ is read now and declares its suites later; see the comment at the top of it for why.
if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
