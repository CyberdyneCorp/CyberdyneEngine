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
        cy::game-backend-ui
        cy::graph
        cy::physics-reference
        cy::navigation
        cy::scene
        cy::servers-input
        cy::servers-audio
        cy::servers-camera
        cy::servers-physics)
add_dependencies(cy_sample_rts-api cy_sample_rts-api_game)

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
                         cy::rendering-pipeline cy::rendering-particles cy::rhi-null
                         cy::rhi-vulkan)
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
    endif()
endif()
