# samples/13-rts-api — an RTS written in Swift against ABI 1.3. `add-swift-game-api`.
#
# The same three parts as samples/04-character: a Swift game module built by SwiftPM, a C++ host
# that loads it over the C ABI, and a test that runs the host as a separate process. The difference
# is the direction of the boundary. 04-character's host carried input, physics, camera and audio
# across components every tick; this host binds the six ABI 1.3 adapters once and the game calls
# the engine itself. See README.md.
#
# Like 04-character, the target is NOT DECLARED without Python 3 or a Swift toolchain: a host with
# no game would be a run that passes on exactly the machines least able to judge it.

# THE RTS TEST MAP (map/, content/rts/): the cook, the loader and their test need neither Swift nor
# a device, so they are declared before the sample's own conditions. The map is a cooked package
# the build writes beside the sample — `cy_rts_map_cook` over content/rts/frontier.cymap — and the
# host draws it with `--render <dir> --map <package>`.
if(TARGET cy::import AND TARGET cy::foliage AND TARGET cy::terrain)
    cy_add_module(
        NAME cy_sample_rts_map
        LAYER tools
        SOURCES
            map/map_format.cpp
            map/map_source.cpp
            map/map_meshes.cpp
            map/map_cook.cpp
            map/map_load.cpp
        PUBLIC_DEPENDENCIES
            cy::core-assets
            cy::core-jobs
            cy::terrain
        PRIVATE_DEPENDENCIES
            cy::environment
            cy::foliage
            cy::import
            cy::world)
    target_include_directories(cy_sample_rts_map PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/map")

    cy_add_module(
        NAME cy_rts_map_cook
        LAYER tools
        TYPE EXECUTABLE
        SOURCES map/cook_main.cpp
        PRIVATE_DEPENDENCIES cy_sample_rts_map)

    set(CY_RTS_MAP_SOURCE "${CMAKE_SOURCE_DIR}/content/rts/frontier.cymap")
    set(CY_RTS_MAP_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/frontier.cypak")
    add_custom_command(
        OUTPUT "${CY_RTS_MAP_PACKAGE}"
        COMMAND cy_rts_map_cook --source "${CY_RTS_MAP_SOURCE}"
                --content-root "${CMAKE_SOURCE_DIR}" --out "${CY_RTS_MAP_PACKAGE}"
        DEPENDS cy_rts_map_cook "${CY_RTS_MAP_SOURCE}"
                "${CMAKE_SOURCE_DIR}/content/beauty/textures/stone_albedo.png"
                "${CMAKE_SOURCE_DIR}/content/beauty/textures/gravel_albedo.png"
                "${CMAKE_SOURCE_DIR}/content/beauty/textures/copper_albedo.png"
        COMMENT "Cooking the RTS test map (content/rts/frontier.cymap)"
        VERBATIM)
    add_custom_target(cy_rts_map_package ALL DEPENDS "${CY_RTS_MAP_PACKAGE}")

    if(CY_BUILD_TESTS)
        get_directory_property(rts_map_budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_integration)
        get_directory_property(rts_map_timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_integration)
        set(CY_TEST_BUDGET_integration "${rts_map_budget}")
        set(CY_TEST_TIMEOUT_integration "${rts_map_timeout}")
        cy_add_test(
            NAME rts_map
            KIND integration
            SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rts_map.cpp"
            DEPENDENCIES cy_sample_rts_map cy::core-base cy::core-memory
            DEFINITIONS
                CY_RTS_MAP_SOURCE="${CY_RTS_MAP_SOURCE}"
                CY_RTS_MAP_CONTENT_ROOT="${CMAKE_SOURCE_DIR}"
                CY_RTS_MAP_PACKAGE="${CY_RTS_MAP_PACKAGE}"
                CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
        add_dependencies(cy_test_integration_rts_map cy_rts_map_package)
    endif()
endif()

# The game builds its HUD through ABI 1.6 and animates its units through ABI 1.7, so the sample is
# not declared without CyberUI or the animation runtime — the way samples/08-vertical-slice and
# samples/09b-animated-character decline without CY_ANIMATION.
if(NOT CY_UI)
    message(STATUS "samples/13-rts-api: CY_UI is off, so the sample is not built")
    return()
endif()
if(NOT CY_ANIMATION)
    message(STATUS "samples/13-rts-api: CY_ANIMATION is off, and the units animate from Swift; "
                   "the sample is not declared.")
    return()
endif()

find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "samples/13-rts-api: no Python 3 interpreter; the sample is not built")
    return()
endif()

set(CY_RTS_API_DRIVER "${CMAKE_SOURCE_DIR}/bindings/swift/tools/cy_swift_module.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CY_RTS_API_DRIVER}" --probe
                RESULT_VARIABLE rts_api_swift_result
                OUTPUT_VARIABLE rts_api_swift_output
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT rts_api_swift_result EQUAL 0)
    message(STATUS "samples/13-rts-api: no Swift toolchain, so the sample is not built. Its game "
                   "IS a Swift module. `just env-doctor` diagnoses this.")
    return()
endif()
message(STATUS "samples/13-rts-api: ${rts_api_swift_output}")

if(CMAKE_BUILD_TYPE STREQUAL "Profile" OR CMAKE_BUILD_TYPE STREQUAL "Shipping")
    set(CY_RTS_API_SWIFT_CONFIGURATION release)
else()
    set(CY_RTS_API_SWIFT_CONFIGURATION debug)
endif()

set(CY_RTS_API_MODULE_DIR "${CMAKE_CURRENT_BINARY_DIR}/module")
set(CY_RTS_API_MODULE_WORK "${CMAKE_CURRENT_BINARY_DIR}/module-build")
set(CY_RTS_API_MODULE_LIBRARY "${CY_RTS_API_MODULE_DIR}/libCyGame_g0.so")
set(CY_RTS_API_MODULE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/module.toml")

# A glob over the DEPENDS of a custom command, not over a target's sources: SwiftPM decides what it
# compiles. samples/04-character/CMakeLists.txt argues the exception at length.
file(GLOB CY_RTS_API_GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/game/*.swift")
file(GLOB_RECURSE CY_RTS_API_KIT_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/bindings/swift/Sources/*")

add_custom_command(
    OUTPUT "${CY_RTS_API_MODULE_LIBRARY}"
    COMMAND "${Python3_EXECUTABLE}" "${CY_RTS_API_DRIVER}"
            --work "${CY_RTS_API_MODULE_WORK}" --out "${CY_RTS_API_MODULE_DIR}"
            --configuration "${CY_RTS_API_SWIFT_CONFIGURATION}"
            --generation "0=${CMAKE_CURRENT_SOURCE_DIR}/game"
    DEPENDS "${CY_RTS_API_DRIVER}" "${CMAKE_SOURCE_DIR}/bindings/swift/Package.swift"
            ${CY_RTS_API_GAME_SOURCES} ${CY_RTS_API_KIT_SOURCES}
    COMMENT "Building the 13-rts-api game module (Swift)"
    VERBATIM)
add_custom_target(cy_sample_rts-api_game ALL DEPENDS "${CY_RTS_API_MODULE_LIBRARY}")

# The host. `just run-sample rts-api` finds it as samples/*/cy_sample_rts-api.
cy_add_module(
    NAME cy_sample_rts-api
    LAYER tools
    TYPE EXECUTABLE
    SOURCES
        host/main.cpp
        host/rts_host.cpp
        host/level.cpp
        host/units.cpp
        host/script.cpp
        host/worker_rig.cpp
        host/illumination.cpp
        host/fog.cpp
    PRIVATE_DEFINITIONS
        "CY_RTS_API_MODULE_LIBRARY=\"${CY_RTS_API_MODULE_LIBRARY}\""
        "CY_RTS_API_MODULE_MANIFEST=\"${CY_RTS_API_MODULE_MANIFEST}\""
    PRIVATE_DEPENDENCIES
        cy::abi
        cy::animation
        cy::animation-assets
        cy::animation-system
        cy::core-assets
        cy::core-jobs
        cy::game-backend
        cy::game-backend-animation
        cy::game-backend-lockstep
        cy::game-backend-ui
        cy::game-backend-illumination
        cy::game-backend-fog
        cy::rendering-gi
        cy::graph
        cy::physics-reference
        cy::navigation
        cy::scene
        cy::servers-input
        cy::servers-audio
        cy::servers-camera
        cy::servers-physics)
add_dependencies(cy_sample_rts-api cy_sample_rts-api_game)

# `--render <dir>` (issue #35, the live half): the session drawn every frame on a device and lit by
# the engine's device GI. The field is the pipeline suites' one scene, compiled here as
# samples/13-rts-selection compiles it, and only with the Vulkan renderer: without it the host is
# the headless one it always was and refuses `--render` by name.
if(CY_RENDERER_VULKAN)
    target_sources(cy_sample_rts-api PRIVATE
                   host/render.cpp
                   "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests/frame_scene.cpp"
                   "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp")
    target_link_libraries(cy_sample_rts-api PRIVATE
                          cy::rendering-gi-gpu cy::rendering-fog-of-war cy::rendering-pipeline
                          cy::rendering-particles
                          cy::rhi cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_sample_rts-api PRIVATE
                               "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests"
                               "${CMAKE_SOURCE_DIR}/tests/render")
    target_compile_definitions(cy_sample_rts-api PRIVATE CY_RTS_API_RENDER=1)
    # `--map <package>`: the session drawn on the RTS test map, which needs the map's loader.
    if(TARGET cy_sample_rts_map)
        target_sources(cy_sample_rts-api PRIVATE host/map_render.cpp)
        target_link_libraries(cy_sample_rts-api PRIVATE
                              cy_sample_rts_map cy::rendering-skinning cy::rendering-occlusion
                              cy::rendering-lighting cy::rendering-material cy::ui-render)
        target_compile_definitions(cy_sample_rts-api PRIVATE CY_RTS_API_MAP=1)
        add_dependencies(cy_sample_rts-api cy_rts_map_package)
    endif()
endif()

# The test runs the host as a separate process, so each run loads the Swift module into a fresh
# image. Declared here rather than in tests/ for 04-character's reason: tests/ is added before
# samples/, and this target does not exist on a machine with no Swift toolchain.
if(CY_BUILD_TESTS)
    get_directory_property(rts_api_budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_integration)
    get_directory_property(rts_api_timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_integration)
    set(CY_TEST_BUDGET_integration "${rts_api_budget}")
    set(CY_TEST_TIMEOUT_integration "${rts_api_timeout}")
    cy_add_test(
        NAME rts_api_sample
        KIND integration
        SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rts_api_sample.cpp"
        DEFINITIONS CY_SAMPLE_RTS_API="$<TARGET_FILE:cy_sample_rts-api>")
    target_include_directories(cy_test_integration_rts_api_sample
                               PRIVATE "${CMAKE_SOURCE_DIR}/tests/smoke")
    add_dependencies(cy_test_integration_rts_api_sample cy_sample_rts-api)

    # render.rts_api_hud: the Swift HUD against samples/13-rts-selection's C++ HUD, on a device.
    # It loads the game module into the test process and draws over the pipeline suites' one scene,
    # so it is declared here, beside the module it loads, and only with the Vulkan renderer.
    if(CY_RENDERER_VULKAN)
        get_directory_property(rts_api_render_budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_render)
        get_directory_property(rts_api_render_timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_render)
        set(CY_TEST_BUDGET_render "${rts_api_render_budget}")
        set(CY_TEST_TIMEOUT_render "${rts_api_render_timeout}")
        set(rts_api_pipeline_tests "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests")
        set(rts_api_selection "${CMAKE_SOURCE_DIR}/samples/13-rts-selection")
        cy_add_test(
            NAME rts_api_hud
            KIND render
            SOURCES
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rts_api_hud_device.cpp"
                "${rts_api_pipeline_tests}/frame_scene.cpp"
                "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
                "${rts_api_selection}/hud.cpp"
            DEPENDENCIES cy::abi cy::game-backend-ui cy::ui-render cy::ui-text
                         cy::rendering-pipeline cy::rendering-particles cy::rendering-fog-of-war
                         cy::rhi-null cy::rhi-vulkan)
        target_include_directories(cy_test_render_rts_api_hud
                                   PRIVATE "${rts_api_pipeline_tests}"
                                           "${CMAKE_SOURCE_DIR}/tests/render"
                                           "${rts_api_selection}")
        target_compile_definitions(cy_test_render_rts_api_hud
                                   PRIVATE
                                       CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
                                       CY_RTS_API_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
                                       CY_RTS_API_MODULE_LIBRARY="${CY_RTS_API_MODULE_LIBRARY}"
                                       CY_RTS_API_MODULE_MANIFEST="${CY_RTS_API_MODULE_MANIFEST}")
        add_dependencies(cy_test_render_rts_api_hud cy_sample_rts-api_game)
        set_tests_properties(render.rts_api_hud PROPERTIES
                             SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")

        # smoke.rts_api_gi (issue #35): the session drawn with the device GI and without it, the
        # beacon the Swift game puts out and lights again seen in the frames, and what a frame
        # costs. It runs the host as a separate process, as integration.rts_api_sample does; a
        # smoke suite, because it draws 420 frames twice.
        get_directory_property(rts_api_smoke_budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_smoke)
        get_directory_property(rts_api_smoke_timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_smoke)
        set(CY_TEST_BUDGET_smoke "${rts_api_smoke_budget}")
        set(CY_TEST_TIMEOUT_smoke "${rts_api_smoke_timeout}")
        cy_add_test(
            NAME rts_api_gi
            KIND smoke
            SOURCES
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rts_api_gi.cpp"
                "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
            DEPENDENCIES cy::core-base cy::core-memory
            DEFINITIONS
                CY_SAMPLE_RTS_API="$<TARGET_FILE:cy_sample_rts-api>"
                CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
        target_include_directories(cy_test_smoke_rts_api_gi
                                   PRIVATE "${CMAKE_SOURCE_DIR}/tests/smoke"
                                           "${CMAKE_SOURCE_DIR}/tests/render")
        add_dependencies(cy_test_smoke_rts_api_gi cy_sample_rts-api)
        set_tests_properties(smoke.rts_api_gi PROPERTIES
                             SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")

        # smoke.rts_api_map: the session drawn on the RTS test map (map/, content/rts/) — the
        # gameplay held to the headless run's, a golden still, and the frame's cost at 1080p.
        # RUN_SERIAL, because the last of its three runs is timed and nothing should share the
        # device with it.
        if(TARGET cy_sample_rts_map)
            cy_add_test(
                NAME rts_api_map
                KIND smoke
                SOURCES
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rts_api_map.cpp"
                    "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
                DEPENDENCIES cy::core-base cy::core-memory
                DEFINITIONS
                    CY_SAMPLE_RTS_API="$<TARGET_FILE:cy_sample_rts-api>"
                    CY_RTS_MAP_PACKAGE="${CY_RTS_MAP_PACKAGE}"
                    CY_RTS_API_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
                    CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
            target_include_directories(cy_test_smoke_rts_api_map
                                       PRIVATE "${CMAKE_SOURCE_DIR}/tests/smoke"
                                               "${CMAKE_SOURCE_DIR}/tests/render")
            add_dependencies(cy_test_smoke_rts_api_map cy_sample_rts-api cy_rts_map_package)
            set_tests_properties(smoke.rts_api_map PROPERTIES
                                 SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine"
                                 RUN_SERIAL TRUE)
        endif()

        # smoke.rts_api_fog (ABI 1.10): the session drawn with blue's fog of war and without it,
        # the enemy camp drawn by blue's sight, and what the stage costs. A separate process, as
        # smoke.rts_api_gi is, for the same reason.
        cy_add_test(
            NAME rts_api_fog
            KIND smoke
            SOURCES
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rts_api_fog.cpp"
                "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
            DEPENDENCIES cy::core-base cy::core-memory
            DEFINITIONS
                CY_SAMPLE_RTS_API="$<TARGET_FILE:cy_sample_rts-api>"
                CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
        target_include_directories(cy_test_smoke_rts_api_fog
                                   PRIVATE "${CMAKE_SOURCE_DIR}/tests/smoke"
                                           "${CMAKE_SOURCE_DIR}/tests/render")
        add_dependencies(cy_test_smoke_rts_api_fog cy_sample_rts-api)
        set_tests_properties(smoke.rts_api_fog PROPERTIES
                             SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
    endif()
endif()
