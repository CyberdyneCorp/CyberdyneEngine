# src/rendering/assembly/tests/ — the frame, assembled. M8.b task 11.2.
#
# TWO SUITES, AND THE SPLIT IS THE TAXONOMY'S, WHICH IS ABOUT COST.
#
#   render_assembly     KIND integration. Every case builds a real ECS world, a real scene tree, a
#                       real snapshot and a real render graph, and the last of them executes the
#                       frame through the NULL backend. A world, a scene tree and a device are each
#                       named in the taxonomy's own description of what does NOT belong in `unit`,
#                       and this suite has all three — so it is integration, deliberately, and not
#                       a unit case that happens to run fast on this machine.
#
#   assembly            KIND render. The same frame on a Vulkan device with validation and
#                       synchronisation validation on, with the GPU cull dispatch attached. Declared
#                       ONLY when CY_RENDERER_VULKAN is on, for the reason tests/render/ gives: a
#                       suite that always skips is a suite whose failure nobody would notice. With
#                       the backend compiled in and no GPU on the machine it SKIPS LOUDLY.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing, exactly as
# src/rendering/gpu_culling/tests/CMakeLists.txt does it.
set_property(GLOBAL PROPERTY CY_ASSEMBLY_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_assembly_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_ASSEMBLY_TESTS_DIR)

    foreach(kind IN ITEMS integration render)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/assembly/tests: the '${kind}' entry of the test taxonomy could not "
                "be read from tests/CMakeLists.txt. If the budget variables were renamed, this "
                "lookup and the deferral around it should go away — declare the suites from "
                "tests/integration/ instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME render_assembly
        KIND integration
        SOURCES "${tests_dir}/test_assembly.cpp"
        DEPENDENCIES cy::rendering-assembly cy::rendering-scene cy::rhi-null cy::scene)

    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    cy_add_test(
        NAME assembly
        KIND render
        SOURCES "${tests_dir}/test_assembly_device.cpp"
        DEPENDENCIES cy::rendering-assembly cy::rhi-null cy::rhi-vulkan
                     cy::servers-render-virtual-texturing)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_assembly_declare_tests)
