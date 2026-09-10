# src/ui/ — CyberUI: element storage, layout, `.cyss`, input routing, flattening and accessibility.
# M8.b section 9.
#
# UI ELEMENTS ARE NOT ENTITIES, AND THE DEPENDENCY LIST IS THE PROOF. `ui-system`: "UI elements SHALL
# be stored in a dedicated, data-oriented UI store addressed by a lightweight `UIElementID`, not as
# ECS entities", with the scenario "UI elements SHALL not appear in the ECS world at all". This
# module does not link cy::ecs — not "does not create entities", cannot name one — so the layer
# checker and the link graph keep the requirement rather than a reviewer.
#
# WHAT IT DOES NOT DEPEND ON, AND WHY EACH ABSENCE IS DELIBERATE.
#   cy::ecs             see above.
#   cy::servers-text    text measurement arrives through `ContentMeasurer`, an interface the caller
#                       implements over `TextServer`. A layout module that included a font server
#                       could not be tested without one, and this one is tested with a measurer that
#                       returns numbers.
#   any renderer        `flatten()` produces a primitive stream. Uploading it is the renderer's, and
#                       a UI module that named a device would be a UI module with a device in its
#                       tests.
#
# THIS WHOLE DIRECTORY IS BEHIND CY_UI, and it did not used to be. `ui-system` says "The system
# SHALL be removable at build time via CY_UI"; the reading this file carried until M8.b's closing
# gate was that the option gates the RUNTIME'S USE of the interface, the line CY_VIRTUAL_GEOMETRY
# draws — but there is no runtime use site to gate, because the whole of CyberUI is engine code with
# no library behind it and no renderer above it yet. So the option removed nothing, defaulted off,
# and was never once built either way: `-D CY_UI=OFF` and the default build produced identical
# trees. The requirement asks for removability, so the option now removes the module and its suites,
# which is where CY_ANIMATION draws the same line, and `m8b:feature-options-off` builds it.
#
# LAYER. `scene` (4), beside src/graph/ and src/world/.
cy_add_module(
    NAME cy_ui
    LAYER scene
    ALIAS cy::ui
    TYPE STATIC
    SOURCES
        src/interaction.cpp
        src/layout.cpp
        src/paint.cpp
        src/store.cpp
        src/style.cpp
        src/widgets.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::core-values)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
