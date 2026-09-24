# samples/10-world — M10's artefact. Seven modules, one world, one video.
#
# AN ELEVENTH SHAPE, and the shape is the milestone's own claim: one program in which the seven
# things M10 built are wired to each other rather than each to its own suite.
#
#   cy_sample_world         the program: generate, cook, claim, place, simulate, draw, and a
#                           numbered PNG per frame.
#   just capture-world      the recipe that runs it and turns the sequence into an mp4.
#
# ================================================================================================
# WHY THIS TARGET'S DEPENDENCY LIST IS THE POINT
# ================================================================================================
#
# M10 landed `src/environment/`, `src/pcg/`, `src/terrain/`, `src/water/`, `src/foliage/`,
# `src/weather/` and the atmosphere half of `src/rendering/sky/`, and every one of them was reached
# only from its own tests. This line is what removes that sentence from the tree: one target names
# all seven, and the seams between them either line up or this program does not run.
#
# It also names what the modules themselves deliberately do NOT name. `cy::weather` does not link
# `cy::water`; `cy::water` does not link `cy::terrain`; `cy::pcg` links neither `cy::foliage` nor
# `cy::ecs`; `cy::rendering-sky` is not linked by any of them. Those absences are requirements made
# checkable by the link graph, and the wiring has to happen SOMEWHERE — an artefact is where.
#
# ================================================================================================
# WHAT M11.c ADDED TO THIS LIST, AND WHY IT IS THE POINT OF THAT RUNG'S SECTION 3
# ================================================================================================
#
# M11.c's own ledger states the defect in as many words: "`samples/10-world` links neither
# `cy::rendering-assembly` nor the post, temporal, forward or shadow libraries, so the world picture
# never passes through tone mapping or anti-aliasing at all." Two entries close it, and they bring
# the rest with them because `cy::rendering-assembly` links all eight of the modules M7's gate found
# unreachable:
#
#   cy::rendering-assembly   the frame: the post chain decides the feature set, the temporal
#                            framework advances one jitter, the shadow cache spends its budget, the
#                            sky table updates, and the stages go into the graph in order.
#   cy::rendering-pipeline   the tonemapping resolve — `cy/fullscreen.slang`'s own, the same one
#                            every other caller of the layer binds.
#
# WHAT IS STILL NOT CLAIMED. There is no temporal resolve in this tree for `FramePassKind::Temporal`
# to record, so this program runs no anti-aliasing stage and its manifest says so. stage.h's header
# carries the rest of the list.
#
# ================================================================================================
# THE NATIVE BACKEND IS A LINK-TIME CHOICE
# ================================================================================================
#
# Apple builds link and select native Metal; other supported desktop builds select Vulkan. Turning
# either backend off still leaves the headless authoritative simulation available, while a request
# for rendered frames fails when no native device answers.

# `--quiet-host` is tools/quiet-host's check (M11.c's seventh close moved it there, so that the test
# suites' ledger criteria could share it). tools/ is behind CY_BUILD_TOOLS, and this sample is not:
# a build without the tools still gets the library, from the same directory.
if(NOT TARGET cy::host-load)
    add_subdirectory("${CMAKE_SOURCE_DIR}/tools/quiet-host" "${CMAKE_BINARY_DIR}/tools/quiet-host")
endif()

cy_add_module(
    NAME cy_sample_world
    LAYER tools
    TYPE EXECUTABLE
    SOURCES
        main.cpp
        world.cpp
        stage.cpp
        "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
    PRIVATE_DEPENDENCIES
        cy::environment
        cy::foliage
        cy::host-load
        cy::pcg
        cy::pcg-adapters
        cy::rendering-assembly
        cy::rendering-graph
        cy::rendering-pipeline
        cy::rendering-sky
        cy::rhi
        cy::rhi-null
        cy::terrain
        cy::water
        cy::weather
        cy::world)

target_include_directories(cy_sample_world
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                   "${CMAKE_SOURCE_DIR}/tests/render")

if(CY_RENDERER_VULKAN)
    target_link_libraries(cy_sample_world PRIVATE cy::rhi-vulkan)
endif()
if(CY_RENDERER_METAL)
    target_link_libraries(cy_sample_world PRIVATE cy::rhi-metal)
endif()

# ================================================================================================
# THE PICTURE HAS NO CTest ENTRY HERE, AND THAT IS DELIBERATE
# ================================================================================================
#
# The picture needs a graphics device, and just/run.just:366's note about `capture-virtual-geometry`
# states the rule this follows: "It needs a graphics device. On a machine without one the tool says
# so and writes nothing, which is why this is a recipe a person runs and not a test." A suite that
# skipped on every machine without one would be a suite whose failure nobody would notice.
#
# What IS gated automatically is everything underneath it: `unit.environment`,
# `integration.environment_gpu`, `unit.terrain`, `integration.terrain_*`, `unit.water`,
# `integration.water_*`, `unit.foliage`, `integration.foliage_*`, `unit.weather`,
# `integration.weather_*`, `unit.pcg`, `integration.pcg_*` and `unit.sky` each test one of the parts
# this artefact composes.

# ================================================================================================
# TWO ENTRIES THAT NEED NO DEVICE: THE QUIET-HOST CHECK, UNDER A HOST MADE BUSY ON PURPOSE
# ================================================================================================
#
# `m11a:world-budget-on-a-device` measures on a quiet host and fails, saying "host too busy:", when
# the host is not. `quiet_host_test.py` runs this binary HEADLESS, so every machine can judge it,
# with four niced spinners it starts on purpose and kills by PID, and requires the run to fail with
# that reason. Removing either check turns its entry red:
#
#   * `smoke.world_quiet_host_before` loads the host BEFORE the take. Judged on every host: a busy
#     host is exactly what it needs, so it can never be "not evaluated".
#   * `smoke.world_quiet_host_across` loads the host once the take has STARTED. It needs the pre-take
#     check to pass first, which needs a host that is quiet to begin with; on a busy one it exits 3,
#     SKIP_RETURN_CODE, and says why. It is a separate entry so that its skip never hides the other
#     leg's verdict.
#
# RUN_SERIAL because both load four cores on purpose and no other suite should share the machine.
if(NOT CY_BUILD_TESTS)
    return()
endif()
find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "samples/10-world: smoke.world_quiet_host_* need python3 and Linux's /proc, so "
                   "they are not declared.")
    return()
endif()
get_directory_property(CY_WORLD_QUIET_TIMEOUT DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                       DEFINITION CY_TEST_TIMEOUT_smoke)
if(NOT CY_WORLD_QUIET_TIMEOUT)
    message(FATAL_ERROR
        "samples/10-world: the test taxonomy's timeouts could not be read from "
        "tests/CMakeLists.txt. If those variables were renamed, this lookup should move with them.")
endif()
foreach(leg IN ITEMS before across)
    add_test(NAME smoke.world_quiet_host_${leg}
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/quiet_host_test.py"
                     --sample "$<TARGET_FILE:cy_sample_world>" --leg ${leg}
             WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(smoke.world_quiet_host_${leg} PROPERTIES
                         LABELS "smoke" TIMEOUT "${CY_WORLD_QUIET_TIMEOUT}" RUN_SERIAL TRUE)
endforeach()
set_tests_properties(smoke.world_quiet_host_across PROPERTIES SKIP_RETURN_CODE 3)
