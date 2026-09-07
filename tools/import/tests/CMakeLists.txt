# tools/import/tests/ — the import pipeline's suites. M5 task 5.1.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* tools/, so that function does not exist while this file is read. The
# declaration is therefore deferred to the end of the top-level directory's processing, and the
# taxonomy is read back out of tests/ rather than restated. Every module with its own suites does
# the same, for the same reason.
#
# THREE SUITES, SPLIT BY COST RATHER THAN BY SUBJECT.
#
#   `import` is unit: options, the JSON reader and texture import. All of it is in memory, none of it
#   opens a file, and every case fits a millisecond in every profile.
#
#   `import_mesh` and `import_gltf` are integration, and they are there rather than in `import` for a
#   reason worth writing down. Welding, cache ordering and quadric-error simplification over a mesh
#   large enough to assert anything cost 1.3 to 1.8 ms of CPU in the **Debug** profile, and a glTF
#   imported twice to prove its output is byte-identical costs about 1.2 ms — against the unit
#   taxonomy's one-millisecond budget. All of them fit in dev, in profile and in release, and a suite
#   that passes in three configurations and fails in the fourth is exactly the shape this project has
#   paid for before. `testing-and-quality` says where such a test goes — "the next suite up" — so
#   they went there, rather than being shrunk until the assertions stopped being worth making.
#
#   `import_pipeline` is integration: it writes a project to a scratch directory, cooks it, reads the
#   cache back, changes a dependency and cooks it again. Every case touches a real filesystem, which
#   is exactly what makes them worth having and what puts them over a millisecond.

set_property(GLOBAL PROPERTY CY_IMPORT_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_import_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_IMPORT_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "tools/import/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
                "the deferral around it should go away — declare the suites from tests/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME import
        KIND unit
        SOURCES
            "${tests_dir}/test_importer_key.cpp"
            "${tests_dir}/test_json.cpp"
            "${tests_dir}/test_options.cpp"
            "${tests_dir}/test_texture.cpp"
        DEPENDENCIES cy::import)

    cy_add_test(
        NAME import_mesh
        KIND integration
        SOURCES "${tests_dir}/test_mesh.cpp"
        DEPENDENCIES cy::import)

    cy_add_test(
        NAME import_gltf
        KIND integration
        SOURCES "${tests_dir}/test_gltf.cpp"
        DEPENDENCIES cy::import)

    # `import_fbx` is integration for the same reason `import_gltf` is: ufbx parses a real document
    # and the mesh is then welded, tangent-generated and optimised, and the determinism case imports
    # the same file twice to compare every byte. That is over the unit taxonomy's millisecond in the
    # Debug profile.
    cy_add_test(
        NAME import_fbx
        KIND integration
        SOURCES "${tests_dir}/test_fbx.cpp"
        DEPENDENCIES cy::import)

    # `import_unwrap` covers the xatlas-backed lightmap unwrap, the overdraw reorder and the convex
    # decomposition — the three mesh-processing steps M6 added. Integration because an unwrap of a
    # mesh large enough to produce more than one chart is milliseconds of work by construction.
    cy_add_test(
        NAME import_unwrap
        KIND integration
        SOURCES "${tests_dir}/test_unwrap.cpp"
        DEPENDENCIES cy::import)

    cy_add_test(
        NAME import_pipeline
        KIND integration
        SOURCES "${tests_dir}/test_pipeline.cpp"
        DEPENDENCIES cy::import cy::core-assets cy::core-jobs)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_import_declare_tests)
