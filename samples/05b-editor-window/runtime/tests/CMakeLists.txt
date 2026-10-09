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
            "${tests_dir}/../service_queue.cpp"
        DEPENDENCIES cy::servers-render cy::abi)

    # The physics debug layers and the joint gizmo, rasterised into a frame from the reference
    # solver's own debug output and an authored world. No device, like the gizmo's case above.
    # INTEGRATION, NOT UNIT: each case builds a reference physics server and rasterises 320x180
    # frames, which costs 2 to 6 ms of CPU at -O0 against the unit tier's 4 ms Debug budget. It
    # failed `four-profiles`' Debug row and about one run in ten alone; tests/harness's rule for a
    # case within a factor of two of its budget is to move it up a tier, as M7 did for the overlay
    # clipping case.
    cy_add_test(
        NAME editor_window_physics_overlay
        KIND integration
        SOURCES
            "${tests_dir}/test_physics_overlay.cpp"
            "${tests_dir}/../overlay.cpp"
            "${tests_dir}/../physics_overlay.cpp"
        DEPENDENCIES cy::servers-render cy::physics-reference cy::gameplay-play
                     cy::scene-serialization)

    cy_add_test(
        NAME editor_wind_field_preview
        KIND integration
        SOURCES
            "${tests_dir}/test_wind_field_preview.cpp"
            "${tests_dir}/../wind_field_preview.cpp"
        DEPENDENCIES cy::weather cy::environment)

    if(CY_VFX)
        cy_add_test(
            NAME editor_scene_vfx
            KIND integration
            SOURCES
                "${tests_dir}/test_scene_vfx_runtime.cpp"
                "${tests_dir}/../scene_vfx_runtime.cpp"
            DEPENDENCIES cy::vfx cy::scene cy::scene-serialization
            DEFINITIONS
                CY_EDITOR_WINDOW_HAS_VFX=1
                "CY_SAMPLE_PROJECT=\"${CMAKE_SOURCE_DIR}/samples/05b-editor-window/project\"")
        target_include_directories(cy_test_integration_editor_scene_vfx PRIVATE "${tests_dir}/..")
    endif()

    # THE AUTHORED FRAME ON LINUX'S DEVICE TOO. Since PR #14 the hosted runtime draws every opened
    # world through `AuthoredFrame`, so the pixel assertions that prove it on Metal are the ones
    # that prove the Linux viewport. Issue #18: nothing ran them on Vulkan.
    if(LINUX AND CY_RENDERER_VULKAN)
        cy_add_test(
            NAME editor_authored_frame_vulkan
            KIND smoke
            SOURCES
                "${tests_dir}/test_authored_frame.cpp"
                "${tests_dir}/../authored_frame.cpp"
                "${tests_dir}/../material_runtime.cpp"
                "${tests_dir}/../wind_field_preview.cpp"
                "${tests_dir}/../world_view.cpp"
            DEPENDENCIES
                cy::rendering-pipeline cy::rendering-skinning cy::import cy::core-assets cy::graph-material
                cy::sample-first-light cy::scene-serialization cy::scene cy::editor-backend
                cy::rendering-material-slang cy::shader
                cy::weather
                cy::rhi-vulkan cy::rhi-null
            DEFINITIONS
                "CY_TEST_PROJECT=\"${CMAKE_SOURCE_DIR}\""
                # material_runtime.cpp mounts the staged shader tree whether or not Slang is built,
                # as cy_editor_window_runtime does.
                "CY_SHADER_STAGE_PARENT=\"${CMAKE_SOURCE_DIR}/src/rendering\"")
        target_include_directories(cy_test_smoke_editor_authored_frame_vulkan PRIVATE
                                   "${tests_dir}/.." "${CMAKE_SOURCE_DIR}/tests/render")
        # The PNG encoder, for the terrain case's opt-in viewport photograph.
        target_sources(cy_test_smoke_editor_authored_frame_vulkan PRIVATE
                       "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp")
        target_compile_definitions(cy_test_smoke_editor_authored_frame_vulkan PRIVATE
                                   CY_EDITOR_WINDOW_HAS_GOLDEN=1)
        # #112's gaps: a project's imported character drawn skinned, read from its cooked
        # directory through the hosted runtime's own reader.
        if(CY_ANIMATION)
            target_sources(cy_test_smoke_editor_authored_frame_vulkan PRIVATE
                           "${tests_dir}/../animation_assets.cpp")
            target_include_directories(cy_test_smoke_editor_authored_frame_vulkan PRIVATE
                                       "${CMAKE_SOURCE_DIR}/src/editor_backend/tests")
            target_compile_definitions(cy_test_smoke_editor_authored_frame_vulkan PRIVATE
                CY_EDITOR_WINDOW_TEST_PROJECT_CHARACTER=1
                "CY_TEST_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
        endif()
        if(CY_SHADER_SLANG)
            target_link_libraries(cy_test_smoke_editor_authored_frame_vulkan PRIVATE
                                  cy::shader-slang)
            target_compile_definitions(cy_test_smoke_editor_authored_frame_vulkan PRIVATE
                                       CY_SHADER_SLANG=1)
        endif()
    endif()

    if(APPLE AND CY_RENDERER_METAL AND CY_SHADER_SLANG)
        cy_add_test(
            NAME editor_authored_frame_metal
            KIND smoke
            SOURCES
                "${tests_dir}/test_authored_frame.cpp"
                "${tests_dir}/../authored_frame.cpp"
                "${tests_dir}/../material_runtime.cpp"
                "${tests_dir}/../wind_field_preview.cpp"
                "${tests_dir}/../world_view.cpp"
            DEPENDENCIES
                cy::rendering-pipeline cy::rendering-skinning cy::import cy::core-assets cy::graph-material
                cy::sample-first-light cy::scene-serialization cy::scene cy::editor-backend
                cy::rendering-material-slang cy::shader cy::shader-slang
                cy::weather
                cy::rhi-metal cy::rhi-null
            DEFINITIONS
                "CY_TEST_PROJECT=\"${CMAKE_SOURCE_DIR}\""
                "CY_SHADER_STAGE_PARENT=\"${CMAKE_SOURCE_DIR}/src/rendering\""
                CY_SHADER_SLANG=1)
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
                                       CY_EDITOR_WINDOW_HAS_VFX=1 CY_EDITOR_WINDOW_HAS_GOLDEN=1)
        endif()

        cy_add_test(
            NAME editor_material_metal
            KIND smoke
            SOURCES
                "${tests_dir}/test_material_metal.cpp"
                "${tests_dir}/../material_runtime.cpp"
                "${tests_dir}/../wind_field_preview.cpp"
                "${tests_dir}/../world_view.cpp"
            DEPENDENCIES
                cy::sample-first-light cy::scene-serialization cy::scene cy::editor-backend
                cy::rendering-material-slang cy::shader cy::shader-slang cy::rhi-metal
                cy::rhi-null cy::weather
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

    # NAVIGATION AUTHORING, WITHOUT A DEVICE. Issue #28, tasks 3.1 to 3.6: the seam over a
    # `.cyworld` map through the composite binding the runtime installs, the incremental updates and
    # sidecar reloads a document sync triggers, navmesh picking against a recorded frame view, and
    # the overlay's image test on the CPU canvas. INTEGRATION because every case voxelises a bake.
    cy_add_test(
        NAME editor_window_navigation
        KIND integration
        SOURCES
            "${tests_dir}/test_nav_overlay.cpp"
            "${tests_dir}/test_nav_runtime.cpp"
            "${tests_dir}/../nav_overlay.cpp"
            "${tests_dir}/../nav_runtime.cpp"
        DEPENDENCIES cy::editor-backend cy::navigation cy::import cy::core-assets
                     cy::scene-serialization cy::scene cy::servers-render
        DEFINITIONS
            "CY_NAV_TEST_DATA=\"${tests_dir}/data\"")
    target_include_directories(cy_test_integration_editor_window_navigation PRIVATE
                               "${tests_dir}/.."
                               "${CMAKE_SOURCE_DIR}/src/editor_backend/tests"
                               "${CMAKE_SOURCE_DIR}/src/navigation/tests")

    # #84 and #29: THE PLAY DEBUGGER AND HOT RELOAD THROUGH THE HOSTED RUNTIME'S GLUE, without a
    # device or a Swift toolchain. A graph breakpoint must pause the whole `PlaySession`, and the
    # debugged run must be the undebugged one placement for placement. Integration because each
    # case runs a reference physics server through a few hundred ticks.
    cy_add_test(
        NAME editor_window_graph_debugger
        KIND integration
        SOURCES "${tests_dir}/test_graph_debugger_runtime.cpp"
                "${tests_dir}/../graph_runtime.cpp"
        DEPENDENCIES cy::abi cy::gameplay-play cy::physics-reference cy::scene-serialization
                     cy::scene cy::game-backend cy::editor-backend
        DEFINITIONS
            "CY_SCRIPT_FIXTURE_DIR=\"${CMAKE_SOURCE_DIR}/src/editor_backend/tests/data\"")
    target_include_directories(cy_test_integration_editor_window_graph_debugger PRIVATE
                               "${tests_dir}/..")

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
            file(GLOB_RECURSE spin_package_sources CONFIGURE_DEPENDS
                 "${CMAKE_SOURCE_DIR}/bindings/swift/Sources/*")
            add_custom_command(
                OUTPUT "${spin_module}"
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_SOURCE_DIR}/bindings/swift/tools/cy_swift_module.py"
                        --work "${spin_module_dir}" --out "${spin_module_dir}/out"
                        --generation "1=${tests_dir}/../../project/game"
                DEPENDS "${tests_dir}/../../project/game/SpinCube.swift"
                        "${CMAKE_SOURCE_DIR}/bindings/swift/tools/cy_swift_module.py"
                        ${spin_package_sources}
                VERBATIM)
            add_custom_target(cy_editor_spin_module DEPENDS "${spin_module}")
            cy_add_test(
                NAME editor_script_runtime
                KIND smoke
                SOURCES "${tests_dir}/test_script_runtime.cpp"
                        "${tests_dir}/../script_runtime.cpp"
                DEPENDENCIES cy::abi cy::gameplay-play cy::physics-reference
                             cy::scene-serialization cy::scene cy::game-backend
                DEFINITIONS "CY_SPIN_MODULE=\"${spin_module}\"")
            if(CY_VFX)
                target_sources(cy_test_smoke_editor_script_runtime PRIVATE
                               "${tests_dir}/../scene_vfx_runtime.cpp")
                target_link_libraries(cy_test_smoke_editor_script_runtime PRIVATE cy::vfx)
                target_compile_definitions(
                    cy_test_smoke_editor_script_runtime PRIVATE
                    CY_EDITOR_WINDOW_HAS_VFX=1
                    "CY_VFX_SAMPLE_PROJECT=\"${tests_dir}/../../project\"")
            endif()
            add_dependencies(cy_test_smoke_editor_script_runtime cy_editor_spin_module)
            target_include_directories(cy_test_smoke_editor_script_runtime PRIVATE
                                       "${tests_dir}/..")
        endif()
    endif()

    # #112's gaps: a graph baked for an imported character and played by Play's animation, the
    # events placed on the Animation panel's timeline arriving on their ticks. INTEGRATION because
    # it imports an FBX, writes a project and runs a Play session. With the animation runtime and
    # the importer only.
    if(CY_ANIMATION AND TARGET cy::import)
        cy_add_test(
            NAME editor_window_play_animation
            KIND integration
            SOURCES
                "${tests_dir}/test_play_animation.cpp"
                "${tests_dir}/../animation_assets.cpp"
                "${tests_dir}/../play_animation.cpp"
            DEPENDENCIES cy::editor-backend cy::import cy::core-assets cy::animation-assets
                         cy::game-backend-animation cy::gameplay-play cy::physics-reference
                         cy::scene-serialization cy::scene cy::abi
            DEFINITIONS
                "CY_TEST_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
        target_include_directories(cy_test_integration_editor_window_play_animation PRIVATE
                                   "${tests_dir}/.." "${CMAKE_SOURCE_DIR}/src/editor_backend/tests")
    endif()

    # #29, VISUAL SCRIPTING: the editor's unit-command graph against its Swift twin. Declared
    # wherever a Swift toolchain is, not only on macOS, because the claim is about the engine's
    # compiled program and the C ABI, neither of which is Apple's. Smoke because it builds and loads
    # a Swift module.
    if(CY_SCRIPTING AND Python3_Interpreter_FOUND)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/bindings/swift/tools/cy_swift_module.py" --probe
            RESULT_VARIABLE twin_swift_result OUTPUT_QUIET ERROR_QUIET)
        if(twin_swift_result EQUAL 0)
            set(twin_module_dir "${CMAKE_CURRENT_BINARY_DIR}/twin-module")
            set(twin_module "${twin_module_dir}/out/libCyGame_g1.so")
            file(GLOB twin_game_sources CONFIGURE_DEPENDS "${tests_dir}/../../project/game/*.swift")
            file(GLOB_RECURSE twin_package_sources CONFIGURE_DEPENDS
                 "${CMAKE_SOURCE_DIR}/bindings/swift/Sources/*")
            add_custom_command(
                OUTPUT "${twin_module}"
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_SOURCE_DIR}/bindings/swift/tools/cy_swift_module.py"
                        --work "${twin_module_dir}" --out "${twin_module_dir}/out"
                        --generation "1=${tests_dir}/../../project/game"
                DEPENDS ${twin_game_sources}
                        "${CMAKE_SOURCE_DIR}/bindings/swift/tools/cy_swift_module.py"
                        ${twin_package_sources}
                VERBATIM)
            add_custom_target(cy_editor_twin_module DEPENDS "${twin_module}")
            cy_add_test(
                NAME editor_graph_equivalence
                KIND smoke
                SOURCES "${tests_dir}/test_graph_equivalence.cpp"
                        "${tests_dir}/../script_runtime.cpp"
                        "${tests_dir}/../graph_runtime.cpp"
                DEPENDENCIES cy::abi cy::gameplay-play cy::physics-reference
                             cy::scene-serialization cy::scene cy::game-backend
                             cy::editor-backend
                DEFINITIONS "CY_TWIN_MODULE=\"${twin_module}\""
                            "CY_SCRIPT_FIXTURE_DIR=\"${CMAKE_SOURCE_DIR}/src/editor_backend/tests/data\""
                            "CY_EDITOR_PROJECT_DIR=\"${tests_dir}/../../project\"")
            add_dependencies(cy_test_smoke_editor_graph_equivalence cy_editor_twin_module)
            target_include_directories(cy_test_smoke_editor_graph_equivalence PRIVATE
                                       "${tests_dir}/..")
            # #112's gaps: the timeline's events reaching the project's `AnimatedHero` behaviour
            # in onUpdate, through the same twin module, which builds every project behaviour.
            if(CY_ANIMATION AND TARGET cy::import)
                cy_add_test(
                    NAME editor_animation_events
                    KIND smoke
                    SOURCES "${tests_dir}/test_animation_events.cpp"
                            "${tests_dir}/../script_runtime.cpp"
                            "${tests_dir}/../animation_assets.cpp"
                            "${tests_dir}/../play_animation.cpp"
                    DEPENDENCIES cy::abi cy::gameplay-play cy::physics-reference
                                 cy::scene-serialization cy::scene cy::game-backend
                                 cy::editor-backend cy::import cy::core-assets
                                 cy::animation-assets cy::game-backend-animation
                    DEFINITIONS "CY_TWIN_MODULE=\"${twin_module}\""
                                "CY_TEST_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
                add_dependencies(cy_test_smoke_editor_animation_events cy_editor_twin_module)
                target_include_directories(cy_test_smoke_editor_animation_events PRIVATE
                    "${tests_dir}/.." "${CMAKE_SOURCE_DIR}/src/editor_backend/tests")
            endif()
        endif()
    endif()

    foreach(target cy_test_unit_editor_window_runtime
                   cy_test_integration_editor_window_physics_overlay
                   cy_test_integration_editor_window_overlay
                   cy_test_integration_editor_window_one_world
                   cy_test_integration_editor_window_layout_file)
        target_include_directories(${target} PRIVATE "${tests_dir}/..")
    endforeach()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_editor_window_runtime_declare_tests)
