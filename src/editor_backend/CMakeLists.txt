cy_add_module(
    NAME cy_editor_backend
    LAYER abi
    ALIAS cy::editor-backend
    TYPE STATIC
    SOURCES src/material_service.cpp src/navigation_service.cpp src/composite_service.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    # cy::navigation is layer 4 (scene) under this layer-6 service: the navigation service runs
    # the bake and the queries, and its header names the navigation types it exchanges.
    PUBLIC_DEPENDENCIES cy::abi cy::graph-material cy::navigation)

if(CY_VFX)
    target_link_libraries(cy_editor_backend PRIVATE cy::vfx)
    target_compile_definitions(cy_editor_backend PRIVATE CY_EDITOR_HAS_VFX=1)
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
