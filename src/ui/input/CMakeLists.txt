# src/ui/input/ — the toolkit driven by the input server: the UI.* actions, their default bindings
# in a mapping context, and the bridge that delivers them and sets the input user's focus layer, so
# gameplay is suppressed while the interface holds the input. Issue #91, stage 4.
#
# ITS OWN TARGET so `cy::ui` and the toolkit still link no input server: a suite drives the toolkit
# with `UiAction`s directly, and only this target and its suite need the server.
#
# LAYER. `scene` (4), over `cy::servers-input` (2).
cy_add_module(
    NAME cy_ui_input
    LAYER scene
    ALIAS cy::ui-input
    TYPE STATIC
    SOURCES
        src/ui_input.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::ui-toolkit cy::servers-input)
