# samples/14-third-person — a third-person game written in Swift, drawn by the engine's frame.
#
# The same three parts as samples/13-rts-api: a Swift game module built by SwiftPM, a C++ host that
# loads it over the C ABI and binds the adapters the game calls, and tests that run the host as a
# separate process. The level is a cooked package: content/third-person/hillside.cymap, cooked by
# the RTS test map's cook (`cy_rts_map_cook`, samples/13-rts-api/map/) into the build tree beside
# the sample, and read back through the asset system. See README.md.
#
# NOT DECLARED without what it is made of: a Swift toolchain (the game IS a Swift module), the
# animation runtime (the hero's blend space and feet), Jolt (the terrain is a triangle mesh and the
# ramp a turned box, which the reference backend collides as their bounds), and the map's cook.

if(NOT CY_ANIMATION OR NOT CY_PHYSICS)
    message(STATUS "samples/14-third-person: CY_ANIMATION and CY_PHYSICS are both needed (the "
                   "hero's animation, the Jolt world it walks on); the sample is not declared.")
    return()
endif()
if(NOT TARGET cy_rts_map_cook OR NOT TARGET cy_sample_rts_map)
    message(STATUS "samples/14-third-person: the map cook (samples/13-rts-api/map) is not built; "
                   "the sample is not declared.")
    return()
endif()

find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "samples/14-third-person: no Python 3 interpreter; the sample is not built")
    return()
endif()

set(CY_THIRD_PERSON_DRIVER "${CMAKE_SOURCE_DIR}/bindings/swift/tools/cy_swift_module.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CY_THIRD_PERSON_DRIVER}" --probe
                RESULT_VARIABLE third_person_swift_result
                OUTPUT_VARIABLE third_person_swift_output
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT third_person_swift_result EQUAL 0)
    message(STATUS "samples/14-third-person: no Swift toolchain, so the sample is not built. Its "
                   "game IS a Swift module. `just env-doctor` diagnoses this.")
    return()
endif()
message(STATUS "samples/14-third-person: ${third_person_swift_output}")

# THE LEVEL: the hillside cooked by the RTS map's cook, a pure function of its source and the
# content images it names.
set(CY_THIRD_PERSON_SOURCE "${CMAKE_SOURCE_DIR}/content/third-person/hillside.cymap")
set(CY_THIRD_PERSON_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/hillside.cypak")
add_custom_command(
    OUTPUT "${CY_THIRD_PERSON_PACKAGE}"
    COMMAND cy_rts_map_cook --source "${CY_THIRD_PERSON_SOURCE}"
            --content-root "${CMAKE_SOURCE_DIR}" --out "${CY_THIRD_PERSON_PACKAGE}"
    DEPENDS cy_rts_map_cook "${CY_THIRD_PERSON_SOURCE}"
            "${CMAKE_SOURCE_DIR}/content/beauty/textures/stone_albedo.png"
            "${CMAKE_SOURCE_DIR}/content/beauty/textures/gravel_albedo.png"
            "${CMAKE_SOURCE_DIR}/content/beauty/textures/copper_albedo.png"
    COMMENT "Cooking the third-person level (content/third-person/hillside.cymap)"
    VERBATIM)
add_custom_target(cy_third_person_package ALL DEPENDS "${CY_THIRD_PERSON_PACKAGE}")

if(CMAKE_BUILD_TYPE STREQUAL "Profile" OR CMAKE_BUILD_TYPE STREQUAL "Shipping")
    set(CY_THIRD_PERSON_SWIFT_CONFIGURATION release)
else()
    set(CY_THIRD_PERSON_SWIFT_CONFIGURATION debug)
endif()

set(CY_THIRD_PERSON_MODULE_DIR "${CMAKE_CURRENT_BINARY_DIR}/module")
set(CY_THIRD_PERSON_MODULE_WORK "${CMAKE_CURRENT_BINARY_DIR}/module-build")
set(CY_THIRD_PERSON_MODULE_LIBRARY "${CY_THIRD_PERSON_MODULE_DIR}/libCyGame_g0.so")
set(CY_THIRD_PERSON_MODULE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/module.toml")

# A glob over the DEPENDS of a custom command, not over a target's sources: SwiftPM decides what it
# compiles. samples/04-character/CMakeLists.txt argues the exception at length.
file(GLOB CY_THIRD_PERSON_GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/game/*.swift")
file(GLOB_RECURSE CY_THIRD_PERSON_KIT_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/bindings/swift/Sources/*")

add_custom_command(
    OUTPUT "${CY_THIRD_PERSON_MODULE_LIBRARY}"
    COMMAND "${Python3_EXECUTABLE}" "${CY_THIRD_PERSON_DRIVER}"
            --work "${CY_THIRD_PERSON_MODULE_WORK}" --out "${CY_THIRD_PERSON_MODULE_DIR}"
            --configuration "${CY_THIRD_PERSON_SWIFT_CONFIGURATION}"
            --generation "0=${CMAKE_CURRENT_SOURCE_DIR}/game"
    DEPENDS "${CY_THIRD_PERSON_DRIVER}" "${CMAKE_SOURCE_DIR}/bindings/swift/Package.swift"
            ${CY_THIRD_PERSON_GAME_SOURCES} ${CY_THIRD_PERSON_KIT_SOURCES}
    COMMENT "Building the 14-third-person game module (Swift)"
    VERBATIM)
add_custom_target(cy_sample_third-person_game ALL DEPENDS "${CY_THIRD_PERSON_MODULE_LIBRARY}")

# The host. `just run-sample third-person` finds it as samples/*/cy_sample_third-person.
cy_add_module(
    NAME cy_sample_third-person
    LAYER tools
    TYPE EXECUTABLE
    SOURCES
        host/main.cpp
        host/tps_host.cpp
        host/level.cpp
        host/script.cpp
        host/hero_rig.cpp
        host/hero_mesh.cpp
        host/sounds.cpp
        host/illumination.cpp
    PRIVATE_DEFINITIONS
        "CY_THIRD_PERSON_MODULE_LIBRARY=\"${CY_THIRD_PERSON_MODULE_LIBRARY}\""
        "CY_THIRD_PERSON_MODULE_MANIFEST=\"${CY_THIRD_PERSON_MODULE_MANIFEST}\""
        "CY_THIRD_PERSON_PACKAGE=\"${CY_THIRD_PERSON_PACKAGE}\""
    PRIVATE_DEPENDENCIES
        cy::abi
        cy::animation
        cy::animation-assets
        cy::animation-ground
        cy::animation-system
        cy::core-assets
        cy::core-jobs
        cy::game-backend
        cy::game-backend-animation
        cy::graph
        cy::physics-jolt
        cy::rendering-gi
        cy::scene
        cy::servers-audio
        cy::servers-camera
        cy::servers-input
        cy::servers-physics
        cy_sample_rts_map)
add_dependencies(cy_sample_third-person cy_sample_third-person_game cy_third_person_package)
# The played game's pad bindings (samples/play/play_bindings.h), header-only.
target_include_directories(cy_sample_third-person PRIVATE "${CMAKE_SOURCE_DIR}/samples/play")

# `--render <dir>`: the run drawn by the engine's frame (host/render.h). Only with the Vulkan
# renderer; without it the host is the headless one and refuses `--render` by name.
if(CY_RENDERER_VULKAN)
    target_sources(cy_sample_third-person PRIVATE
                   host/render.cpp
                   "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp")
    target_link_libraries(cy_sample_third-person PRIVATE
                          cy::rendering-gi-gpu cy::rendering-pipeline cy::rendering-skinning
                          cy::rendering-occlusion cy::rendering-lighting cy::rendering-material
                          cy::rendering-upscale cy::rhi cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_sample_third-person PRIVATE "${CMAKE_SOURCE_DIR}/tests/render")
    target_compile_definitions(cy_sample_third-person PRIVATE CY_THIRD_PERSON_RENDER=1)
endif()

# The tests run the host as a separate process, so each run loads the module into a fresh image.
# Declared here rather than in tests/ for 04-character's reason: tests/ is added before samples/,
# and this target does not exist on a machine with no Swift toolchain.
# `--play`: the hillside in a window, played with a mouse, a keyboard or a pad, through the desktop
# play host (samples/play) — drawn when the build has the Vulkan renderer, driven with no picture
# (`--no-draw`) when it has not.
if(TARGET cy::sample-play)
    target_link_libraries(cy_sample_third-person PRIVATE cy::sample-play)
    target_compile_definitions(cy_sample_third-person PRIVATE CY_THIRD_PERSON_PLAY=1)
endif()

if(CY_BUILD_TESTS)
    get_directory_property(third_person_budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_integration)
    get_directory_property(third_person_timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_integration)
    set(CY_TEST_BUDGET_integration "${third_person_budget}")
    set(CY_TEST_TIMEOUT_integration "${third_person_timeout}")
    cy_add_test(
        NAME third_person_sample
        KIND integration
        SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_third_person_sample.cpp"
        DEFINITIONS CY_SAMPLE_THIRD_PERSON="$<TARGET_FILE:cy_sample_third-person>")
    target_include_directories(cy_test_integration_third_person_sample
                               PRIVATE "${CMAKE_SOURCE_DIR}/tests/smoke")
    add_dependencies(cy_test_integration_third_person_sample cy_sample_third-person)

    # smoke.third_person_render: the run drawn — gameplay held to the headless run's, a golden
    # still, and the frame's cost at 1080p. A smoke suite, because it draws 1200 frames twice;
    # RUN_SERIAL, because the last run is timed and nothing should share the device with it.
    if(CY_RENDERER_VULKAN)
        get_directory_property(third_person_smoke_budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_smoke)
        get_directory_property(third_person_smoke_timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_smoke)
        set(CY_TEST_BUDGET_smoke "${third_person_smoke_budget}")
        set(CY_TEST_TIMEOUT_smoke "${third_person_smoke_timeout}")
        cy_add_test(
            NAME third_person_render
            KIND smoke
            SOURCES
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_third_person_render.cpp"
                "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
            DEPENDENCIES cy::core-base cy::core-memory
            DEFINITIONS
                CY_SAMPLE_THIRD_PERSON="$<TARGET_FILE:cy_sample_third-person>"
                CY_THIRD_PERSON_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
                CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
        target_include_directories(cy_test_smoke_third_person_render
                                   PRIVATE "${CMAKE_SOURCE_DIR}/tests/smoke"
                                           "${CMAKE_SOURCE_DIR}/tests/render")
        add_dependencies(cy_test_smoke_third_person_render cy_sample_third-person)
        set_tests_properties(smoke.third_person_render PROPERTIES
                             SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine"
                             RUN_SERIAL TRUE)
    endif()
endif()
