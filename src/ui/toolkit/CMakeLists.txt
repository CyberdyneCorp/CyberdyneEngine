# src/ui/toolkit/ — the widget toolkit: a retained widget tree on CyberUI's store, its widgets,
# input, focus, styles, animation, bindings and documents. Issue #91, stages 4 and 5.
#
# ITS OWN TARGET, beside cy::ui, for the reason text/ and render/ are: `cy::ui` names no font server,
# and a toolkit's labels, captions and text fields are text. This target links `cy::ui-text` and
# nothing that draws: a widget is elements, and the stream they flatten to is drawn by
# `cy::ui-render` exactly as any other.
#
# LAYER. `scene` (4), beside cy::ui.
cy_add_module(
    NAME cy_ui_toolkit
    LAYER scene
    ALIAS cy::ui-toolkit
    TYPE STATIC
    SOURCES
        src/animation.cpp
        src/document.cpp
        src/text_edit.cpp
        src/toolkit.cpp
        src/toolkit_document.cpp
        src/toolkit_frame.cpp
        src/toolkit_input.cpp
        src/toolkit_style.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::ui cy::ui-text cy::core-assets)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
