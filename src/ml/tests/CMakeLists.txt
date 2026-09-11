# src/ml/tests/ — CyberML's suites. M8.c section 4.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing and the taxonomy is read back out
# of tests/ rather than restated, the same way src/ai/tests/ and src/audio/tests/ do it.
#
#   ml                  the model: shapes and tensors, the asset and its container, the registry and
#                       the selection rule, sessions over a backend that is not a runtime, and the
#                       budget's arithmetic. UNIT: no world, no thread, no file.
#
#   ml_cook             the cook-time determinism boundary, driven over real model assets. UNIT for
#                       the same reason: it is a few dozen plain values through one gate.
#
#   ml_runtime          CyberML beside the job system and the ECS firewall: a session's write origin
#                       reaching the enforcement point, dispatch onto worker threads, and teardown
#                       with work in flight. INTEGRATION — it builds a world and starts a pool.
#
#   ml_onnxruntime      the reference backend running the committed model. INTEGRATION, and it is
#                       DECLARED ONLY WHEN THE OPTION IS ON rather than registered and skipped: a
#                       suite that reports "passed" while running nothing is the failure this
#                       project has already paid for four times.

set_property(GLOBAL PROPERTY CY_ML_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set_property(GLOBAL PROPERTY CY_ML_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../assets")

function(cy_ml_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_ML_TESTS_DIR)
    get_property(assets_dir GLOBAL PROPERTY CY_ML_ASSETS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/ml/tests: the '${kind}' entry of the test taxonomy could not be read from "
                "tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME ml
        KIND unit
        SOURCES
            "${tests_dir}/test_backend.cpp"
            "${tests_dir}/test_model.cpp"
            "${tests_dir}/test_schedule.cpp"
            "${tests_dir}/test_tensor.cpp"
        DEPENDENCIES cy::ml)

    cy_add_test(
        NAME ml_cook
        KIND unit
        SOURCES "${tests_dir}/test_cook.cpp"
        DEPENDENCIES cy::ml-cook)

    cy_add_test(
        NAME ml_runtime
        KIND integration
        SOURCES "${tests_dir}/test_runtime.cpp"
        DEPENDENCIES cy::ml cy::ecs cy::core-reflect)

    if(CY_ML_ONNXRUNTIME)
        cy_add_test(
            NAME ml_onnxruntime
            KIND integration
            SOURCES "${tests_dir}/test_onnxruntime.cpp"
            DEPENDENCIES cy::ml
            DEFINITIONS "CY_ML_TEST_ASSET_DIR=\"${assets_dir}\"")
    endif()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_ml_declare_tests)
