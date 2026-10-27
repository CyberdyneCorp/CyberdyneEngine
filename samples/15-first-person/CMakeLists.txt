# samples/15-first-person — a first-person shooter written in Swift against ABI 1.15.
#
# The same three parts as samples/13-rts-api: a Swift game module built by SwiftPM, a C++ host that
# loads it over the C ABI, and tests that run the host as a separate process. The game moves a
# character, looks through a first-person camera with a weapon view, shoots by physics raycast,
# pushes props and hurts targets, and leaves a flash, bursts, sparks and scorch marks; the targets
# are the engine's AI; with `--render` the session is drawn with the GI, the upscaler, shadows,
# ambient occlusion, decals and particles. See README.md.
#
# NOT DECLARED without Swift or Python 3 (a host with no game would be a run that passes on exactly
# the machines least able to judge it), nor without the AI or the VFX system, which the targets and
# the effects are.
if(NOT CY_AI)
    message(STATUS "samples/15-first-person: CY_AI is off, and the targets are the engine's AI; "
                   "the sample is not declared.")
    return()
endif()
if(NOT CY_PHYSICS)
    message(STATUS "samples/15-first-person: CY_PHYSICS is off, and a shot pushes crates that have "
                   "to land and collide; the reference backend resolves no contacts. The sample is "
                   "not declared.")
    return()
endif()
if(NOT CY_VFX)
    message(STATUS "samples/15-first-person: CY_VFX is off, and a shot's bursts are VFX systems; "
                   "the sample is not declared.")
    return()
endif()

find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "samples/15-first-person: no Python 3 interpreter; the sample is not built")
    return()
endif()

set(CY_FIRST_PERSON_DRIVER "${CMAKE_SOURCE_DIR}/bindings/swift/tools/cy_swift_module.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CY_FIRST_PERSON_DRIVER}" --probe
                RESULT_VARIABLE first_person_swift_result
                OUTPUT_VARIABLE first_person_swift_output
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT first_person_swift_result EQUAL 0)
    message(STATUS "samples/15-first-person: no Swift toolchain, so the sample is not built. Its "
                   "game IS a Swift module. `just env-doctor` diagnoses this.")
    return()
endif()
message(STATUS "samples/15-first-person: ${first_person_swift_output}")

if(CMAKE_BUILD_TYPE STREQUAL "Profile" OR CMAKE_BUILD_TYPE STREQUAL "Shipping")
    set(CY_FIRST_PERSON_SWIFT_CONFIGURATION release)
else()
    set(CY_FIRST_PERSON_SWIFT_CONFIGURATION debug)
endif()

set(CY_FIRST_PERSON_MODULE_DIR "${CMAKE_CURRENT_BINARY_DIR}/module")
set(CY_FIRST_PERSON_MODULE_WORK "${CMAKE_CURRENT_BINARY_DIR}/module-build")
set(CY_FIRST_PERSON_MODULE_LIBRARY "${CY_FIRST_PERSON_MODULE_DIR}/libCyGame_g0.so")
set(CY_FIRST_PERSON_MODULE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/module.toml")

# A glob over the DEPENDS of a custom command, not over a target's sources: SwiftPM decides what it
# compiles. samples/04-character/CMakeLists.txt argues the exception at length.
file(GLOB CY_FIRST_PERSON_GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/game/*.swift")
file(GLOB_RECURSE CY_FIRST_PERSON_KIT_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/bindings/swift/Sources/*")

add_custom_command(
    OUTPUT "${CY_FIRST_PERSON_MODULE_LIBRARY}"
    COMMAND "${Python3_EXECUTABLE}" "${CY_FIRST_PERSON_DRIVER}"
            --work "${CY_FIRST_PERSON_MODULE_WORK}" --out "${CY_FIRST_PERSON_MODULE_DIR}"
            --configuration "${CY_FIRST_PERSON_SWIFT_CONFIGURATION}"
            --generation "0=${CMAKE_CURRENT_SOURCE_DIR}/game"
    DEPENDS "${CY_FIRST_PERSON_DRIVER}" "${CMAKE_SOURCE_DIR}/bindings/swift/Package.swift"
            ${CY_FIRST_PERSON_GAME_SOURCES} ${CY_FIRST_PERSON_KIT_SOURCES}
    COMMENT "Building the 15-first-person game module (Swift)"
    VERBATIM)
add_custom_target(cy_sample_first-person_game ALL DEPENDS "${CY_FIRST_PERSON_MODULE_LIBRARY}")

# The host. `just run-sample first-person` finds it as samples/*/cy_sample_first-person.
cy_add_module(
    NAME cy_sample_first-person
    LAYER tools
    TYPE EXECUTABLE
    SOURCES
        host/main.cpp
        host/arena.cpp
        host/bodies.cpp
        host/content.cpp
        host/fps_host.cpp
        host/illumination.cpp
        host/script.cpp
        host/shot_effects.cpp
        host/targets.cpp
        # `StageBuilder`, the graph builder every VFX suite authors its effects with.
        "${CMAKE_SOURCE_DIR}/src/vfx/tests/effects.cpp"
    PRIVATE_DEFINITIONS
        "CY_FIRST_PERSON_MODULE_LIBRARY=\"${CY_FIRST_PERSON_MODULE_LIBRARY}\""
        "CY_FIRST_PERSON_MODULE_MANIFEST=\"${CY_FIRST_PERSON_MODULE_MANIFEST}\""
    PRIVATE_DEPENDENCIES
        cy::abi
        cy::ai
        cy::graph
        cy::game-backend
        cy::game-backend-decals
        cy::game-backend-effects
        cy::game-backend-first-person
        cy::game-backend-lights
        cy::physics-jolt
        cy::rendering-decals
        cy::rendering-gi
        cy::rendering-lighting
        cy::rendering-particles
        cy::scene
        cy::servers-audio
        cy::servers-camera
        cy::servers-input
        cy::servers-physics
        cy::vfx
        cy::vfx-compiler)
target_include_directories(cy_sample_first-person PRIVATE "${CMAKE_SOURCE_DIR}/src/vfx/tests")
add_dependencies(cy_sample_first-person cy_sample_first-person_game)

# `--render <dir>`: the session drawn every frame on a device through the engine's frame — the GI,
# the upscaler, the weapon view, decals, particles, the flashes — and only with the Vulkan renderer:
# without it the host is the headless one and refuses `--render` by name.
if(CY_RENDERER_VULKAN)
    target_sources(cy_sample_first-person PRIVATE
                   host/render.cpp
                   host/render_content.cpp
                   "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp")
    target_link_libraries(cy_sample_first-person PRIVATE
                          cy::rendering-assembly cy::rendering-gi-gpu cy::rendering-material
                          cy::rendering-occlusion cy::rendering-pipeline cy::rendering-upscale
                          cy::rendering-viewmodel cy::servers-render cy::rhi cy::rhi-null
                          cy::rhi-vulkan)
    target_include_directories(cy_sample_first-person PRIVATE "${CMAKE_SOURCE_DIR}/tests/render")
    target_compile_definitions(cy_sample_first-person PRIVATE
                               CY_FIRST_PERSON_RENDER=1
                               "CY_FIRST_PERSON_CONTENT_ROOT=\"${CMAKE_SOURCE_DIR}\"")
    # The arena's textures are content/beauty's PNGs, decoded by the importer: a build without the
    # tools draws the same arena in its flat albedos (render_content.h).
    if(TARGET cy::import)
        target_link_libraries(cy_sample_first-person PRIVATE cy::import)
        target_compile_definitions(cy_sample_first-person PRIVATE CY_FIRST_PERSON_TEXTURES=1)
    endif()
endif()

# The tests run the host as a separate process, so each run loads the Swift module into a fresh
# image. Declared here rather than in tests/ for 04-character's reason: tests/ is added before
# samples/, and this target does not exist on a machine with no Swift toolchain.
if(CY_BUILD_TESTS)
    get_directory_property(first_person_budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_integration)
    get_directory_property(first_person_timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_integration)
    set(CY_TEST_BUDGET_integration "${first_person_budget}")
    set(CY_TEST_TIMEOUT_integration "${first_person_timeout}")
    cy_add_test(
        NAME first_person_sample
        KIND integration
        SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_first_person_sample.cpp"
        DEFINITIONS CY_SAMPLE_FIRST_PERSON="$<TARGET_FILE:cy_sample_first-person>")
    target_include_directories(cy_test_integration_first_person_sample
                               PRIVATE "${CMAKE_SOURCE_DIR}/tests/smoke")
    add_dependencies(cy_test_integration_first_person_sample cy_sample_first-person)

    # smoke.first_person_render: the session drawn on a device — the game untouched by the picture,
    # its marks and sparks where its raycasts hit, a golden still, and what a frame costs at 1080p.
    # A smoke suite, because it draws the session five times; RUN_SERIAL, because its last run is
    # timed and nothing should share the device with it.
    if(CY_RENDERER_VULKAN)
        get_directory_property(first_person_smoke_budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_smoke)
        get_directory_property(first_person_smoke_timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_smoke)
        set(CY_TEST_BUDGET_smoke "${first_person_smoke_budget}")
        set(CY_TEST_TIMEOUT_smoke "${first_person_smoke_timeout}")
        cy_add_test(
            NAME first_person_render
            KIND smoke
            SOURCES
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_first_person_render.cpp"
                "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
            DEPENDENCIES cy::core-base cy::core-memory
            DEFINITIONS
                CY_SAMPLE_FIRST_PERSON="$<TARGET_FILE:cy_sample_first-person>"
                CY_FIRST_PERSON_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
                CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
        target_include_directories(cy_test_smoke_first_person_render
                                   PRIVATE "${CMAKE_SOURCE_DIR}/tests/smoke"
                                           "${CMAKE_SOURCE_DIR}/tests/render")
        add_dependencies(cy_test_smoke_first_person_render cy_sample_first-person)
        set_tests_properties(smoke.first_person_render PROPERTIES
                             SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine"
                             RUN_SERIAL TRUE)
    endif()
endif()
