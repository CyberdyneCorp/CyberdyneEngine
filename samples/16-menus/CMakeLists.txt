# samples/16-menus — a game's menus, in Swift, over documents made in the UI layout editor.
#
# Three parts, as samples/13-rts-api: a Swift game module built by SwiftPM, a C++ host that loads it
# over the C ABI, and tests that run the host as a separate process. The documents, the sheet and
# the themes are in project/ui/, written by the UI layout editor from tools/session.py's session.
#
# Not declared without CyberUI or a Swift toolchain: a host with no game, or a game with no
# interface, would be a run that passes on exactly the machines least able to judge it.

if(NOT CY_UI)
    message(STATUS "samples/16-menus: CY_UI is off, so the sample is not built")
    return()
endif()

find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "samples/16-menus: no Python 3 interpreter; the sample is not built")
    return()
endif()

set(CY_MENUS_DRIVER "${CMAKE_SOURCE_DIR}/bindings/swift/tools/cy_swift_module.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CY_MENUS_DRIVER}" --probe
                RESULT_VARIABLE menus_swift_result
                OUTPUT_VARIABLE menus_swift_output
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT menus_swift_result EQUAL 0)
    message(STATUS "samples/16-menus: no Swift toolchain, so the sample is not built. Its game IS "
                   "a Swift module. `just env-doctor` diagnoses this.")
    return()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Profile" OR CMAKE_BUILD_TYPE STREQUAL "Shipping")
    set(CY_MENUS_SWIFT_CONFIGURATION release)
else()
    set(CY_MENUS_SWIFT_CONFIGURATION debug)
endif()

set(CY_MENUS_MODULE_DIR "${CMAKE_CURRENT_BINARY_DIR}/module")
set(CY_MENUS_MODULE_WORK "${CMAKE_CURRENT_BINARY_DIR}/module-build")
set(CY_MENUS_MODULE_LIBRARY "${CY_MENUS_MODULE_DIR}/libCyGame_g0.so")
set(CY_MENUS_MODULE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/module.toml")
set(CY_MENUS_PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/project")

# A glob over the DEPENDS of a custom command, not over a target's sources: SwiftPM decides what it
# compiles. samples/04-character/CMakeLists.txt argues the exception.
file(GLOB CY_MENUS_GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/game/*.swift")
file(GLOB_RECURSE CY_MENUS_KIT_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/bindings/swift/Sources/*")

add_custom_command(
    OUTPUT "${CY_MENUS_MODULE_LIBRARY}"
    COMMAND "${Python3_EXECUTABLE}" "${CY_MENUS_DRIVER}"
            --work "${CY_MENUS_MODULE_WORK}" --out "${CY_MENUS_MODULE_DIR}"
            --configuration "${CY_MENUS_SWIFT_CONFIGURATION}"
            --generation "0=${CMAKE_CURRENT_SOURCE_DIR}/game"
    DEPENDS "${CY_MENUS_DRIVER}" "${CMAKE_SOURCE_DIR}/bindings/swift/Package.swift"
            ${CY_MENUS_GAME_SOURCES} ${CY_MENUS_KIT_SOURCES}
    COMMENT "Building the 16-menus game module (Swift)"
    VERBATIM)
add_custom_target(cy_sample_menus_game ALL DEPENDS "${CY_MENUS_MODULE_LIBRARY}")

# The host. `just run-sample menus` finds it as samples/*/cy_sample_menus. The level the menus sit
# over is the RTS sample's, lit by CyberGI: its illumination is compiled here from that sample.
set(menus_rts_host "${CMAKE_SOURCE_DIR}/samples/13-rts-api/host")
cy_add_module(
    NAME cy_sample_menus
    LAYER tools
    TYPE EXECUTABLE
    SOURCES
        host/main.cpp
        host/menus_host.cpp
        host/script.cpp
        "${menus_rts_host}/illumination.cpp"
    PRIVATE_DEFINITIONS
        "CY_MENUS_MODULE_LIBRARY=\"${CY_MENUS_MODULE_LIBRARY}\""
        "CY_MENUS_MODULE_MANIFEST=\"${CY_MENUS_MODULE_MANIFEST}\""
        "CY_MENUS_PROJECT=\"${CY_MENUS_PROJECT}\""
    PRIVATE_DEPENDENCIES
        cy::abi
        cy::core-assets
        cy::game-backend
        cy::game-backend-ui
        cy::game-backend-illumination
        cy::rendering-gi
        cy::rendering-upscale
        cy::scene
        cy::servers-audio
        cy::servers-input
        cy::ui-input
        cy::ui-text
        cy::ui-toolkit)
target_include_directories(cy_sample_menus PRIVATE "${menus_rts_host}")
add_dependencies(cy_sample_menus cy_sample_menus_game)
# The interface font, with the complete text backend.
if(CY_TEXT)
    target_link_libraries(cy_sample_menus PRIVATE cy::text-complete)
    target_compile_definitions(cy_sample_menus PRIVATE CY_MENUS_INTERFACE_FONT=1)
endif()
# `--render <dir>`: the menus drawn on a device, at the display settings the game applied — only
# with the Vulkan renderer; without it the host is headless and refuses `--render` by name.
if(CY_RENDERER_VULKAN)
    set(menus_pipeline_tests "${CMAKE_SOURCE_DIR}/src/rendering/pipeline/tests")
    target_sources(cy_sample_menus PRIVATE
                   host/render.cpp
                   "${menus_pipeline_tests}/frame_scene.cpp"
                   "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp")
    target_link_libraries(cy_sample_menus PRIVATE
                          cy::rendering-pipeline cy::rendering-particles cy::ui-render
                          cy::rhi cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_sample_menus PRIVATE
                               "${menus_pipeline_tests}" "${CMAKE_SOURCE_DIR}/tests/render")
    target_compile_definitions(cy_sample_menus PRIVATE CY_MENUS_RENDER=1)
endif()

if(CY_BUILD_TESTS)
    get_directory_property(menus_budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_integration)
    get_directory_property(menus_timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_integration)
    set(CY_TEST_BUDGET_integration "${menus_budget}")
    set(CY_TEST_TIMEOUT_integration "${menus_timeout}")
    cy_add_test(
        NAME menus_sample
        KIND integration
        SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_menus_sample.cpp"
        DEPENDENCIES cy::core-base cy::core-memory
        DEFINITIONS
            CY_SAMPLE_MENUS="$<TARGET_FILE:cy_sample_menus>"
            CY_MENUS_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
            CY_MENUS_PROJECT="${CY_MENUS_PROJECT}"
            CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    target_include_directories(cy_test_integration_menus_sample
                               PRIVATE "${CMAKE_SOURCE_DIR}/tests/smoke")
    add_dependencies(cy_test_integration_menus_sample cy_sample_menus)

    if(CY_RENDERER_VULKAN)
        get_directory_property(menus_render_budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_smoke)
        get_directory_property(menus_render_timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_smoke)
        set(CY_TEST_BUDGET_smoke "${menus_render_budget}")
        set(CY_TEST_TIMEOUT_smoke "${menus_render_timeout}")
        cy_add_test(
            NAME menus_frames
            KIND smoke
            SOURCES
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_menus_frames.cpp"
                "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
            DEPENDENCIES cy::core-base cy::core-memory
            DEFINITIONS
                CY_SAMPLE_MENUS="$<TARGET_FILE:cy_sample_menus>"
                CY_MENUS_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
                CY_MENUS_PROJECT="${CY_MENUS_PROJECT}"
                CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
        target_include_directories(cy_test_smoke_menus_frames
                                   PRIVATE "${CMAKE_SOURCE_DIR}/tests/smoke"
                                           "${CMAKE_SOURCE_DIR}/tests/render")
        add_dependencies(cy_test_smoke_menus_frames cy_sample_menus)
        set_tests_properties(smoke.menus_frames PROPERTIES
                             SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine"
                             RUN_SERIAL TRUE)
    endif()
endif()
