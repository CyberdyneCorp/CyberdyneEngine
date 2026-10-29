# samples/play/ — the desktop play host: a window, the platform's input through the game's action
# maps, a real-time loop with fixed-step simulation and interpolated presentation, a pause, the
# interface a game builds through the ABI, and the swapchain the game's picture is presented to.
# samples/15-first-person and samples/14-third-person are played under it with `--play`, and a
# future game implements `cy::play::PlayGame` and gets all of it. See README.md.
#
# A static library at the top of the layer stack, beside the samples that use it: it names the SDL3
# platform, the renderer's graph and the ABI's interface adapter together, which no engine layer
# below `tools` may.
#
# NOT DECLARED without CyberUI: the pause menu and the HUD are the interface, and a played game with
# no way to quit from its menu is not one. The presenter is the Vulkan renderer's; without it the
# host still runs a game in a window with no picture (`--no-draw`), which is how the play loop is
# tested on a machine with no device.
if(NOT TARGET cy::game-backend-ui OR NOT TARGET cy::ui-toolkit)
    message(STATUS "samples/play: CY_UI is off, and a played game's menu is the interface; the "
                   "play host is not declared.")
    return()
endif()

cy_add_module(
    NAME cy_sample_play
    LAYER tools
    ALIAS cy::sample-play
    TYPE STATIC
    SOURCES
        bindings_file.cpp
        input_script.cpp
        pause.cpp
        play_arguments.cpp
        play_clock.cpp
        play_host.cpp
        play_ui.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    PUBLIC_DEPENDENCIES
        cy::core-base
        cy::core-memory
        cy::game-backend-ui
        cy::platform-desktop-sdl3
        cy::platform-host
        cy::rhi
        cy::servers-input
        cy::servers-text
        cy::ui
        cy::ui-render
        cy::ui-text
        cy::ui-toolkit)

# The picture: a device, the window's swapchain, the present pass (shaders/present.slang, its SPIR-V
# and MSL checked in as present_spirv.h and present_msl.h) and the interface over it.
if(CY_RENDERER_VULKAN)
    target_sources(cy_sample_play PRIVATE presenter.cpp)
    target_link_libraries(cy_sample_play PUBLIC cy::rendering-graph cy::rhi-null cy::rhi-vulkan)
    target_compile_definitions(cy_sample_play PUBLIC CY_PLAY_PRESENTER=1)
endif()
# The interface font, a distance field, where the build has the complete text backend.
if(CY_TEXT AND TARGET cy::text-complete)
    target_link_libraries(cy_sample_play PRIVATE cy::text-complete)
endif()

if(CY_BUILD_TESTS)
    get_directory_property(play_unit_budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_BUDGET_unit)
    get_directory_property(play_unit_timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                           DEFINITION CY_TEST_TIMEOUT_unit)
    set(CY_TEST_BUDGET_unit "${play_unit_budget}")
    set(CY_TEST_TIMEOUT_unit "${play_unit_timeout}")
    # The play host's arithmetic and parsing: the clock, the pause, the rebinding file and the input
    # script. No window, no device.
    cy_add_test(
        NAME play_host
        KIND unit
        SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_play_clock.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_play_input.cpp"
        DEPENDENCIES cy::sample-play)
endif()
