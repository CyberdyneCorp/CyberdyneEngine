# src/rendering/minimap/ — layer 4: the topographic minimap. openspec/changes/
# add-rts-combat-and-minimap.
#
# The ground drawn as a map draws it — painted colours, hillshade, contour lines, water and roads —
# baked once on the host, then a team's fog of war over it through fog_of_war's own field sampler,
# then the units and buildings the team may see as icons: an RGBA8 image a HUD shows on an atlas
# page. Host code only, no device and no shader, so every backend draws the same picture.
cy_add_module(
    NAME cy_rendering_minimap
    LAYER rendering
    ALIAS cy::rendering-minimap
    TYPE STATIC
    SOURCES src/topographic.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-fog-of-war)

if(CY_BUILD_TESTS)
    set_property(GLOBAL PROPERTY CY_MINIMAP_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    function(cy_minimap_declare_tests)
        get_property(tests_dir GLOBAL PROPERTY CY_MINIMAP_TESTS_DIR)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_unit)
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_unit)
        set(CY_TEST_BUDGET_unit "${budget}")
        set(CY_TEST_TIMEOUT_unit "${timeout}")
        cy_add_test(
            NAME render_minimap
            KIND unit
            SOURCES "${tests_dir}/test_topographic.cpp"
            DEPENDENCIES cy::rendering-minimap cy::rendering-fog-of-war cy::gameplay-fog)
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_minimap_declare_tests)
endif()
