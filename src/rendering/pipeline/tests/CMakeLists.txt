# src/rendering/pipeline/tests/ — the layer, with and without a device. M8.c tasks 1b.1 and 1b.2,
# and M11.c task 3.7.
#
# THREE SUITES, AND THE FIRST SPLIT IS THE TAXONOMY'S.
#
#   render_pipeline   KIND integration. Every case builds a spatial index, a material table, a
#                     render graph and a null device, and the taxonomy names all four as what does
#                     NOT belong in `unit`. What it asks is what was DECIDED: how many stages carry
#                     a callback, how many ran, how many draws each recorded, and how many bytes the
#                     Prepare transfer moved. All of that is answerable with no GPU.
#
#   pipeline          KIND render. The same scene on Vulkan with validation and synchronisation
#                     validation on, the output image copied back off the device, and the three
#                     pictures this milestone commits written out. Declared ONLY when
#                     CY_RENDERER_VULKAN is on, for the reason tests/render/ gives: a suite that
#                     always skips is a suite whose failure nobody would notice. With the backend
#                     compiled in and no GPU it SKIPS LOUDLY.
#
#   forward_material_texture
#                     KIND render, Vulkan only. The same scene with a TEXTURE on its materials,
#                     compared against the same frame with that texture replaced by its declared
#                     average. It is separate from `pipeline` because it needs a device with a
#                     global bindless table and a render server to key residency by, which the
#                     other two need neither of.
#
# THE DEVICE SUITE COMPILES tests/render/golden.cpp. That file is the PNG encoder and the image
# comparison the golden suite already uses, and this one writes its captures with the same encoder
# rather than a second one — a reviewer opens both sets in the same viewer, and there is one decoder
# in the tree that reads what the engine writes.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing, exactly as
# src/rendering/assembly/tests/CMakeLists.txt does it.
set_property(GLOBAL PROPERTY CY_PIPELINE_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_pipeline_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_PIPELINE_TESTS_DIR)

    foreach(kind IN ITEMS integration render)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/pipeline/tests: the '${kind}' entry of the test taxonomy could not "
                "be read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME render_pipeline
        KIND integration
        SOURCES
            "${tests_dir}/frame_scene.cpp"
            "${tests_dir}/test_bloom.cpp"
            "${tests_dir}/test_pipeline.cpp"
        DEPENDENCIES cy::rendering-pipeline cy::rendering-particles cy::rhi-null)
    target_include_directories(cy_test_integration_render_pipeline PRIVATE "${tests_dir}")

    function(cy_pipeline_device_suite name backend define)
        cy_add_test(
            NAME ${name}
            KIND render
            SOURCES
                "${tests_dir}/frame_scene.cpp"
                "${tests_dir}/test_pipeline_device.cpp"
                "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
            DEPENDENCIES cy::rendering-pipeline cy::rendering-particles cy::rhi-null ${backend})
        set(target "cy_test_render_${name}")
        target_include_directories(${target} PRIVATE "${tests_dir}"
                                                   "${CMAKE_SOURCE_DIR}/tests/render")
        target_compile_definitions(${target} PRIVATE ${define}
                                   CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
        set_tests_properties(render.${name} PROPERTIES
                             SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
    endfunction()

    if(CY_RENDERER_VULKAN)
        cy_pipeline_device_suite(pipeline cy::rhi-vulkan CY_TEST_PIPELINE_VULKAN=1)
    endif()
    if(APPLE AND CY_RENDERER_METAL)
        if(CY_RENDERER_VULKAN)
            cy_pipeline_device_suite(pipeline_metal cy::rhi-metal CY_TEST_PIPELINE_METAL=1)
        else()
            cy_pipeline_device_suite(pipeline cy::rhi-metal CY_TEST_PIPELINE_METAL=1)
        endif()
    endif()

    # THE FORWARD PATH WITH A TEXTURE ON IT. M11.c task 3.7's last mile.
    #
    # Vulkan only, and not because Metal is unsupported: the case needs a device with a global
    # bindless table, and `MaterialTextureTable` refuses a device without one by name. It renders
    # the SAME `FrameScene` the two suites above render — one scene in this directory, for the
    # reason the header of this file gives — with one slot of that table bound as every material's
    # base colour, and again with that texture replaced by its declared average.
    #
    # It links cy::servers-render because the residency path is keyed by the render server's own
    # texture handle: the server owns the record and the table owns the image, and `slot_of` is the
    # join. golden.cpp for the PNG encoder, as `render.pipeline` does — one encoder in the tree.
    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()
    cy_add_test(
        NAME forward_material_texture
        KIND render
        SOURCES
            "${tests_dir}/frame_scene.cpp"
            "${tests_dir}/test_forward_material_texture.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::rendering-pipeline cy::rendering-particles cy::servers-render
                     cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_test_render_forward_material_texture
                               PRIVATE "${tests_dir}" "${CMAKE_SOURCE_DIR}/tests/render")
    # WHERE ITS DIAGNOSTIC FRAMES GO, and it is not the caller's working directory. A bare filename
    # landed two PNGs in the repository root, they were committed, and they later blocked a
    # falsifiability proof: `falsify --mutate-the-tree` refuses a dirty tree because a mutation
    # applied to one "could not be told from what is already there".
    target_compile_definitions(cy_test_render_forward_material_texture
                               PRIVATE CY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")

    # BLOOM, MEASURED ON THE LINEAR HDR IT WRITES. KIND render, Vulkan only, for the reason the
    # `pipeline` suite gives. The chain is declared by `declare_bloom_chain` — the function
    # `ForwardFrame` calls — and recorded by `BloomRenderer`, over images uploaded as half floats and
    # read back the same way; its last case renders `FrameScene` with and without bloom.
    cy_add_test(
        NAME bloom
        KIND render
        SOURCES
            "${tests_dir}/frame_scene.cpp"
            "${tests_dir}/test_bloom_device.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::rendering-pipeline cy::rendering-particles cy::rhi-null cy::rhi-vulkan)
    target_include_directories(cy_test_render_bloom PRIVATE "${tests_dir}"
                                                            "${CMAKE_SOURCE_DIR}/tests/render")
    # `references/frame_scene_before_bloom.png`: the scene as `render.pipeline` captured it before
    # bloom existed, which the bloom-off case is compared against.
    target_compile_definitions(cy_test_render_bloom PRIVATE CY_PIPELINE_TEST_DIR="${tests_dir}")
    set_tests_properties(render.bloom PROPERTIES
                         SKIP_REGULAR_EXPRESSION "no requested graphics device on this machine")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_pipeline_declare_tests)
