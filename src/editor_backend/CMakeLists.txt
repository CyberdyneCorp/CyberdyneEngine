cy_add_module(
    NAME cy_editor_backend
    LAYER abi
    ALIAS cy::editor-backend
    TYPE STATIC
    SOURCES
        src/animation_service.cpp
        src/audio_authoring.cpp
        src/audio_service.cpp
        src/composite_service.cpp
        src/material_service.cpp
        src/navigation_service.cpp
        src/script_service.cpp
        src/terrain_service.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    # cy::navigation is layer 4 (scene) under this layer-6 service: the navigation service runs
    # the bake and the queries, and its header names the navigation types it exchanges.
    PUBLIC_DEPENDENCIES
        cy::abi
        cy::graph-material
        cy::navigation
        cy::servers-audio
        cy::game-backend
        cy::scene-serialization
        cy::terrain)

if(CY_VFX)
    target_link_libraries(cy_editor_backend PRIVATE cy::vfx)
    target_compile_definitions(cy_editor_backend PRIVATE CY_EDITOR_HAS_VFX=1)
endif()

# THE ANIMATION PANEL'S PREVIEW CHARACTER (#29). `animation.catalogue.get` and `animation.compile`
# need only cy::graph's pose compiler, which every build has; the character that previews a program
# evaluates it with the animation runtime, which `-D CY_ANIMATION=OFF` removes. PUBLIC, because a
# host that draws the preview names `AnimationPreview` and its skeleton.
if(CY_ANIMATION)
    target_sources(cy_editor_backend PRIVATE src/animation_character.cpp
                                             src/animation_preview.cpp src/animation_rig.cpp)
    target_link_libraries(cy_editor_backend PUBLIC cy::animation cy::animation-assets)
    target_compile_definitions(cy_editor_backend PUBLIC CY_EDITOR_HAS_ANIMATION=1)
endif()

if(CY_BUILD_TESTS)
    set_property(GLOBAL PROPERTY CY_EDITOR_BACKEND_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    function(cy_editor_backend_declare_tests)
        set(editor_backend_test_dependencies cy::editor-backend)
        if(CY_VFX)
            list(APPEND editor_backend_test_dependencies cy::vfx)
        endif()
        get_property(tests_dir GLOBAL PROPERTY CY_EDITOR_BACKEND_TESTS_DIR)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_unit)
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_unit)
        get_directory_property(integration_budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_integration)
        get_directory_property(integration_timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_integration)
        set(CY_TEST_BUDGET_unit "${budget}")
        set(CY_TEST_TIMEOUT_unit "${timeout}")
        set(CY_TEST_BUDGET_integration "${integration_budget}")
        set(CY_TEST_TIMEOUT_integration "${integration_timeout}")
        cy_add_test(
            NAME editor_backend
            KIND unit
            SOURCES "${tests_dir}/test_material_service.cpp"
            DEPENDENCIES ${editor_backend_test_dependencies})
        if(CY_VFX)
            target_compile_definitions(cy_test_unit_editor_backend PRIVATE
                CY_EDITOR_HAS_VFX=1 CY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
        endif()
        # THE AUDIO TOOLS, ENGINE SIDE (#29). `integration` because every case mixes real blocks
        # through the null backend and one reads a WAV file.
        cy_add_test(
            NAME editor_backend_audio
            KIND integration
            SOURCES "${tests_dir}/test_audio_service.cpp"
            DEPENDENCIES cy::editor-backend cy::scene-serialization cy::scene
            DEFINITIONS
                "CY_AUDIO_WIRE_DIR=\"${tests_dir}/data\""
                "CY_TEST_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
        # THE GAMEPLAY GRAPH EDITOR, ENGINE SIDE (#29, visual scripting). `integration` because
        # it reads the committed wire and runs a graph on a scene for four hundred ticks.
        cy_add_test(
            NAME editor_backend_script
            KIND integration
            SOURCES "${tests_dir}/test_script_service.cpp"
            DEPENDENCIES cy::editor-backend cy::game-backend cy::graph cy::scene
            DEFINITIONS "CY_SCRIPT_WIRE_DIR=\"${tests_dir}/data\"")
        # THE ANIMATION PANEL, ENGINE SIDE (#29, animation). `integration` because it reads the
        # committed wire and evaluates the preview character's program against the runtime.
        cy_add_test(
            NAME editor_backend_animation
            KIND integration
            SOURCES "${tests_dir}/test_animation_service.cpp"
            DEPENDENCIES cy::editor-backend cy::graph
            DEFINITIONS "CY_ANIMATION_WIRE_DIR=\"${tests_dir}/data\"")
        # A project's own character arrives through the real FBX importer, as it does in a
        # project: `animation_character_fixture.h` builds the document and imports it. The cases
        # are compiled wherever the importer is (`CY_BUILD_TOOLS`).
        if(TARGET cy::import)
            target_link_libraries(cy_test_integration_editor_backend_animation PRIVATE
                                  cy::import cy::core-assets)
            target_compile_definitions(cy_test_integration_editor_backend_animation PRIVATE
                                       CY_EDITOR_TEST_HAS_IMPORT=1)
        endif()
        cy_add_test(
            NAME editor_backend_terrain
            KIND integration
            SOURCES "${tests_dir}/test_terrain_service.cpp"
            DEPENDENCIES cy::editor-backend cy::terrain)
        target_compile_definitions(cy_test_integration_editor_backend_terrain PRIVATE
            CY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
        cy_add_test(
            NAME editor_backend_compile
            KIND integration
            SOURCES "${tests_dir}/test_material_compile_service.cpp"
            DEPENDENCIES cy::editor-backend)
        target_compile_definitions(cy_test_integration_editor_backend_compile PRIVATE
            CY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
        if(CY_VFX)
            target_sources(cy_test_integration_editor_backend_compile PRIVATE
                "${tests_dir}/test_vfx_preview_service.cpp")
            target_link_libraries(cy_test_integration_editor_backend_compile PRIVATE cy::vfx)
        endif()
        # Issue #28: the navigation service over a fixture seam, and the composite that routes
        # material.* and navigation.* through one binding. INTEGRATION because the bakes voxelise.
        cy_add_test(
            NAME editor_backend_navigation
            KIND integration
            SOURCES "${tests_dir}/test_navigation_service.cpp"
                    "${tests_dir}/test_composite_service.cpp"
            DEPENDENCIES cy::editor-backend cy::navigation)
        target_include_directories(cy_test_integration_editor_backend_navigation PRIVATE
            "${CMAKE_SOURCE_DIR}/src/navigation/tests")
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_editor_backend_declare_tests)
endif()
