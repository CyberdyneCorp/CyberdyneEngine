# samples/05b-editor-window/runtime/tests/ — the runtime's decisions, without a device.
#
# The association, the transaction decoding and the overlay's rasteriser are the parts of this
# runtime that can be wrong quietly: an identity that moves between objects, a transaction half
# applied, a handle drawn in the wrong colour or in the wrong place. None of them needs Vulkan, so
# none of them waits for `smoke.editor_window` — which needs a display and is not in the permanent
# gate set.
set_property(GLOBAL PROPERTY CY_EDITOR_WINDOW_RUNTIME_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_editor_window_runtime_declare_tests)
    find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
    get_property(tests_dir GLOBAL PROPERTY CY_EDITOR_WINDOW_RUNTIME_TESTS_DIR)
    foreach(kind IN ITEMS unit integration smoke)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "samples/05b-editor-window/runtime/tests: the '${kind}' entry of the test taxonomy "
                "could not be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME editor_window_runtime
        KIND unit
        SOURCES
            "${tests_dir}/test_runtime.cpp"
            "${tests_dir}/../overlay.cpp"
            "${tests_dir}/../pick_wire.cpp"
        DEPENDENCIES cy::servers-render)

    if(APPLE AND CY_RENDERER_METAL AND CY_SHADER_SLANG)
        cy_add_test(
            NAME editor_authored_frame_metal
            KIND smoke
            SOURCES
                "${tests_dir}/test_authored_frame_metal.cpp"
                "${tests_dir}/../authored_frame.cpp"
            DEPENDENCIES
                cy::rendering-pipeline cy::import cy::core-assets cy::graph-material
                cy::sample-first-light cy::scene-serialization cy::scene
                cy::rhi-metal
            DEFINITIONS
                "CY_TEST_PROJECT=\"${CMAKE_SOURCE_DIR}\"")
        target_include_directories(cy_test_smoke_editor_authored_frame_metal PRIVATE
                                   "${tests_dir}/..")
        if(CY_VFX)
            target_sources(cy_test_smoke_editor_authored_frame_metal PRIVATE
                           "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp")
            target_include_directories(cy_test_smoke_editor_authored_frame_metal PRIVATE
                                       "${CMAKE_SOURCE_DIR}/tests/render")
            target_link_libraries(cy_test_smoke_editor_authored_frame_metal PRIVATE
                                  cy::vfx cy::rendering-particles cy::editor-backend)
            target_compile_definitions(cy_test_smoke_editor_authored_frame_metal PRIVATE
                                       CY_EDITOR_WINDOW_HAS_VFX=1)
        endif()

        cy_add_test(
            NAME editor_material_metal
            KIND smoke
            SOURCES
                "${tests_dir}/test_material_metal.cpp"
                "${tests_dir}/../material_runtime.cpp"
                "${tests_dir}/../world_view.cpp"
            DEPENDENCIES
                cy::sample-first-light cy::scene-serialization cy::scene cy::editor-backend
                cy::rendering-material-slang cy::shader cy::shader-slang cy::rhi-metal
            DEFINITIONS
                "CY_SAMPLE_PROJECT=\"${tests_dir}/../../project\""
                "CY_SHADER_STAGE_PARENT=\"${CMAKE_SOURCE_DIR}/src/rendering\""
                CY_SHADER_SLANG=1)
        target_include_directories(cy_test_smoke_editor_material_metal PRIVATE "${tests_dir}/..")
    endif()

    # ONE WORLD, END TO END WITHOUT A DEVICE. M8.a tasks 1.1 and 1.4: open the world the editor
    # opened, apply the transactions the editor sends, and resolve a pick against the records the
    # frame produced. It is `integration` rather than `unit` because it builds a scene with 64 box
    # slots and reads a file, which is more than a millisecond of an idle machine's CPU — the same
    # reason `editor_window_overlay` is, and its note says the rest.
    cy_add_test(
        NAME editor_window_one_world
        KIND integration
        SOURCES
            "${tests_dir}/test_one_world.cpp"
            "${tests_dir}/../world_view.cpp"
        DEPENDENCIES cy::servers-render cy::scene-serialization cy::scene cy::sample-first-light
        DEFINITIONS
            "CY_SAMPLE_PROJECT=\"${tests_dir}/../../project\"")

    # The one case in this directory that does not fit a millisecond, and the file says why it
    # cannot be made to. M7's closing gate measured it at 0.81 to 0.98 ms of CPU in the Debug
    # configuration against a 1.00 ms unit budget, and at 1.94 ms on a machine that was building.
    cy_add_test(
        NAME editor_window_overlay
        KIND integration
        SOURCES
            "${tests_dir}/test_overlay_clipping.cpp"
            "${tests_dir}/../overlay.cpp"
        DEPENDENCIES cy::servers-render)

    # THE `--layout` FILE, and the per-frame rewrites that wedged the runtime on a busy disk. See
    # `layout_file.h`. `integration` because it writes a file, which is not a millisecond on a disk
    # that is busy — which is the very condition it was found under.
    cy_add_test(
        NAME editor_window_layout_file
        KIND integration
        SOURCES
            "${tests_dir}/test_layout_file.cpp"
            "${tests_dir}/../layout_file.cpp"
        DEPENDENCIES cy::servers-render
        DEFINITIONS
            "CY_TEST_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")

    if(APPLE AND CY_SCRIPTING AND Python3_Interpreter_FOUND)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/bindings/swift/tools/cy_swift_module.py" --probe
            RESULT_VARIABLE spin_swift_result OUTPUT_QUIET ERROR_QUIET)
        if(spin_swift_result EQUAL 0)
            set(spin_module_dir "${CMAKE_CURRENT_BINARY_DIR}/spin-module")
            set(spin_module "${spin_module_dir}/out/libCyGame_g1.so")
            add_custom_command(
                OUTPUT "${spin_module}"
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_SOURCE_DIR}/bindings/swift/tools/cy_swift_module.py"
                        --work "${spin_module_dir}" --out "${spin_module_dir}/out"
                        --generation "1=${tests_dir}/../../project/game"
                DEPENDS "${tests_dir}/../../project/game/SpinCube.swift"
                        "${CMAKE_SOURCE_DIR}/bindings/swift/tools/cy_swift_module.py"
                VERBATIM)
            add_custom_target(cy_editor_spin_module DEPENDS "${spin_module}")
            cy_add_test(
                NAME editor_script_runtime
                KIND smoke
                SOURCES "${tests_dir}/test_script_runtime.cpp"
                        "${tests_dir}/../script_runtime.cpp"
                DEPENDENCIES cy::abi cy::gameplay-play cy::physics-reference
                             cy::scene-serialization cy::scene
                DEFINITIONS "CY_SPIN_MODULE=\"${spin_module}\"")
            add_dependencies(cy_test_smoke_editor_script_runtime cy_editor_spin_module)
            target_include_directories(cy_test_smoke_editor_script_runtime PRIVATE
                                       "${tests_dir}/..")
        endif()
    endif()

    foreach(target cy_test_unit_editor_window_runtime cy_test_integration_editor_window_overlay
                   cy_test_integration_editor_window_one_world
                   cy_test_integration_editor_window_layout_file)
        target_include_directories(${target} PRIVATE "${tests_dir}/..")
    endforeach()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_editor_window_runtime_declare_tests)
