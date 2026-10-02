# src/ui/console/ — the developer console, built on CyberUI.
#
# `ui-system`'s forcing functions ask for in-game tooling shipped on the runtime interface rather
# than demonstrated beside it, a developer console first. This is that console: elements in an
# `ElementStore`, its text through `cy::ui-text`, drawn by whatever draws the rest of the
# interface. It names no device: `cy::ui-render` draws it, and `unit.ui_console` runs it with none.
#
# LAYER. `scene` (4), beside cy::ui.
cy_add_module(
    NAME cy_ui_console
    LAYER scene
    ALIAS cy::ui-console
    TYPE STATIC
    SOURCES src/console.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::ui cy::ui-text)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
