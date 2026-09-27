# tests/render/ — the render suites. Section 5 of the M3 tasks.
#
# The directory has held a README and nothing else since M0, which named this milestone as the one
# that would fill it. What lands here first is section 5: the conventions, asserted on a device.
#
#   render.conventions   reversed-Z sampled back from the depth buffer, camera-relative rendering at
#                        a million units, and the axis conventions checked against rendered output.
#   render.golden        the milestone's artefact photographed and compared against a committed
#                        reference (task 6.3), plus the same frame through the null backend for a
#                        structure-for-structure comparison (task 7.1).
#   render.frames        MANY frames on the device, so a per-frame defect cannot survive to an
#                        artefact as M3's recycled descriptor did (M4 task 1.3).
#   render.null_frame    that same frame with NO GPU AT ALL (task 6.4).
#   render.xr_prerequisites  the three deferred-scope checks tests/render/README.md has recorded
#                        since M0 (task 6.5).
#
# THE GOLDEN-IMAGE SUITE JOINS THE CONVENTIONS SUITE, it does not replace it. The two answer
# different questions: a golden image says "the frame looks right", and the conventions cases say
# "the depth comparison is the right way round" — which a golden image cannot say, because a scene
# renders correctly under an inverted comparison until something intersects (design.md §3).
#
# TWO GROUPS, AND THE DIVIDING LINE IS WHETHER A DEVICE IS NEEDED.
#
#   conventions, golden and frames need one to sample back from and to photograph, so they are
#   declared only when CY_RENDERER_VULKAN is on — not declared and skipped, because a suite that
#   always skips is a suite whose failure nobody would notice — and they SKIP loudly on a machine
#   that has the backend compiled in and no GPU, naming the backend that was selected instead.
#
#   null_frame and xr_prerequisites are declared ALWAYS. Everything they check is decided rather than
#   drawn — which passes survive, how many barriers the derivation emitted, whether two runs agree,
#   whether stereo is one submission — and design.md §1's whole argument is that those questions must
#   be answerable on a machine with no GPU, which is most continuous-integration machines. A
#   rendering milestone whose only gate is a photograph is a rendering milestone that is not gated on
#   the machines that build it.
set_property(GLOBAL PROPERTY CY_RENDER_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_render_declare_tests)
    get_property(cy_render_tests_dir GLOBAL PROPERTY CY_RENDER_TESTS_DIR)

    # --- The two suites that need no device ------------------------------------------------------
    #
    # Both link cy::sample-first-light, the library half of samples/03-first-light: what they run is
    # the milestone's own renderer rather than a scene written for a test, so a change that breaks
    # the artefact turns a gate red rather than being noticed the next time somebody runs it by
    # hand. See samples/03-first-light/CMakeLists.txt for why the sample is split that way.
    cy_add_test(
        NAME null_frame
        KIND render
        SOURCES "${cy_render_tests_dir}/test_null_frame.cpp"
        DEPENDENCIES cy::sample-first-light cy::rhi-null)

    cy_add_test(
        NAME xr_prerequisites
        KIND render
        SOURCES "${cy_render_tests_dir}/test_xr_prerequisites.cpp"
        DEPENDENCIES cy::sample-first-light cy::rhi-null)

    # EVERY ENABLED DRAWING BACKEND, AND WHICH ONE PRODUCED WHICH IMAGE. M11.d task 7.3 and
    # M11.d.5 task 2.6. This suite is also the native Metal golden test on Apple hosts.
    set(cy_render_golden_backend_dependencies
        cy::sample-first-light cy::rhi-null)
    if(CY_RENDERER_VULKAN)
        list(APPEND cy_render_golden_backend_dependencies cy::rhi-vulkan)
    endif()
    if(CY_RENDERER_METAL)
        list(APPEND cy_render_golden_backend_dependencies cy::rhi-metal)
    endif()
    if(CY_RENDERER_D3D12)
        list(APPEND cy_render_golden_backend_dependencies cy::rhi-d3d12)
    endif()
    if(CY_RENDERER_VULKAN OR CY_RENDERER_METAL OR CY_RENDERER_D3D12)
        cy_add_test(
            NAME golden_backends
            KIND render
            SOURCES
                "${cy_render_tests_dir}/golden.cpp"
                "${cy_render_tests_dir}/test_golden_backends.cpp"
            DEPENDENCIES ${cy_render_golden_backend_dependencies}
            DEFINITIONS CY_RENDER_TEST_DIR="${cy_render_tests_dir}")
        target_include_directories(cy_test_render_golden_backends PRIVATE "${cy_render_tests_dir}")
    endif()

    if(NOT CY_RENDERER_VULKAN)
        return()
    endif()

    # --- The three suites that need one -----------------------------------------------------------
    cy_add_test(
        NAME golden
        KIND render
        SOURCES
            "${cy_render_tests_dir}/golden.cpp"
            "${cy_render_tests_dir}/test_golden_frame.cpp"
        DEPENDENCIES cy::sample-first-light cy::rhi-null cy::rhi-vulkan
        # The reference images live beside their test rather than in the build tree, and CTest runs a
        # suite from its BINARY directory — so the source directory is compiled in rather than
        # reached by a relative path that would depend on where the run started.
        DEFINITIONS CY_RENDER_TEST_DIR="${cy_render_tests_dir}")

    # MANY FRAMES, task 1.3 of M4's carried-forward debt. Its own suite rather than another case in
    # `render.golden`, because it needs no reference image at all: it compares the run against
    # itself, which is an exact claim rather than one with a tolerance, and cannot be laundered by
    # regenerating a golden file. See test_many_frames.cpp for what one-frame suites could not see.
    cy_add_test(
        NAME frames
        KIND render
        SOURCES "${cy_render_tests_dir}/test_many_frames.cpp"
        DEPENDENCIES cy::sample-first-light cy::rhi-null cy::rhi-vulkan)

    cy_add_test(
        NAME conventions
        KIND render
        SOURCES
            "${cy_render_tests_dir}/probe.cpp"
            "${cy_render_tests_dir}/test_axis_conventions.cpp"
            "${cy_render_tests_dir}/test_camera_relative.cpp"
            "${cy_render_tests_dir}/test_reversed_z.cpp"
        DEPENDENCIES cy::core-math cy::rendering-graph cy::rhi-null cy::rhi-vulkan)
    # The probe includes "probe.h" and "shaders/conventions_spirv.h" by relative path, which needs
    # this directory on the include path — the sources are compiled from the top-level binary
    # directory, so a relative include finds nothing without it.
    target_include_directories(cy_test_render_conventions PRIVATE "${cy_render_tests_dir}")

    # THE ENVIRONMENT FIELD ON A DEVICE. `m10:fields-sampled-on-a-device` measured the number of
    # `.slang` modules sampling an environment field at zero; `cy/field.slang` is the module and this
    # is the suite that runs it on a graphics device against `sample_field_image()` over the same
    # bytes. KIND render for the reason every suite in this directory is: it needs a device, and the
    # render taxonomy's budget and timeout are the ones written for a suite that talks to a GPU.
    #
    # It lives here rather than in src/environment/tests/ because `cy::environment` is a layer-2
    # module that may not name the renderer or a device. A suite under tests/ is layer 7 and may name
    # anything, which is what keeps `just quality-layers` green with a device-side field sampler in
    # the tree — the invariant m11a.toml's `layering` criterion exists to hold.
    # THE RAY-TRACING CAPABILITY, ON A REAL DEVICE. M11.c tasks 2.6 and 2.7. `unit.rhi` drives the
    # DERIVATION and needs no GPU; what it cannot check is whether the Vulkan backend fills the
    # observation truthfully, because a derivation over a struct the test wrote is a test of that
    # struct. This suite opens a device and compares the capability against what the driver
    # answered. KIND render because it needs one.
    cy_add_test(
        NAME ray_tracing_capability
        KIND render
        SOURCES "${cy_render_tests_dir}/test_ray_tracing_device.cpp"
        DEPENDENCIES cy::rendering-raytracing cy::rhi cy::rhi-null cy::rhi-vulkan)
    # For "device.h", as the conventions and environment-field suites below.
    target_include_directories(cy_test_render_ray_tracing_capability PRIVATE "${cy_render_tests_dir}")

    # THE MATERIAL TEXTURE BINDING, ON A REAL DEVICE. M11.c task 3.7. The RHI's global bindless
    # table was created, written and destroyed and reached no pipeline layout and no command buffer
    # in the whole tree — so every index it handed out named a descriptor no shader could read.
    # Nothing device-free can tell that table from a working one: the handles are non-null either
    # way, the pipeline layout is created either way, and only a sampled texel says which. KIND
    # render because it needs a GPU, and it skips loudly without one like every suite here.
    cy_add_test(
        NAME material_binding
        KIND render
        SOURCES "${cy_render_tests_dir}/test_material_binding.cpp"
        DEPENDENCIES cy::rendering-graph cy::rendering-pipeline cy::rhi cy::rhi-null cy::rhi-vulkan
                     cy::servers-render)
    # For "device.h" and "shaders/material_probe_spirv.h", as the suites above.
    target_include_directories(cy_test_render_material_binding PRIVATE "${cy_render_tests_dir}")

    # THE SKY, AS AN IMAGE. M11.c task 5.2, `m11c:sky-as-an-image`. The row has been measured by
    # five criteria over four suites and every one of them reads a NUMBER; the requirement is about
    # what the sky LOOKS like, which a table cannot carry. Four times of day, each compared against
    # a committed reference and against the other three — see test_sky_times_of_day.cpp for why one
    # would not do and for what is on the processor and what is on the device.
    #
    # It compiles golden.cpp for the same comparison, the same PNG encoder and the same derived
    # tolerance `render.golden` uses. A second image comparison would be a second tolerance to keep
    # honest.
    cy_add_test(
        NAME sky_times_of_day
        KIND render
        SOURCES
            "${cy_render_tests_dir}/golden.cpp"
            "${cy_render_tests_dir}/test_sky_times_of_day.cpp"
        DEPENDENCIES cy::rendering-graph cy::rendering-sky cy::rhi cy::rhi-null cy::rhi-vulkan
                     cy::world
        # The references live beside their test, as `render.golden`'s do, and CTest runs a suite
        # from its BINARY directory.
        DEFINITIONS CY_RENDER_TEST_DIR="${cy_render_tests_dir}")
    # For "device.h", "golden.h" and "shaders/sky_frame_spirv.h", as the suites above.
    target_include_directories(cy_test_render_sky_times_of_day PRIVATE "${cy_render_tests_dir}")

    # THE SHADED VIRTUAL-GEOMETRY FRAME. M11.c task 4, `m11c:virtual-geometry-image`. The row's
    # published picture was a normals debug view written by `just capture-virtual-geometry` — a
    # recipe a person runs, compared by nothing — so the picture was asserted by nobody. This suite
    # photographs `samples/07-fidelity`'s OWN frame through `cy::sample-fidelity-frame`, shades it
    # through `shade.cpp` (the same function the capture recipe calls, so the compared picture and
    # the published one cannot drift), and compares it against a committed reference.
    #
    # It compiles golden.cpp for the same reason `render.sky_times_of_day` does: one PNG encoder,
    # one comparison and one derived tolerance in this directory, not three.
    #
    # The second case starves the shadow page cache to one slot and asserts the frame CHANGES, which
    # is what stops a reference that had quietly stopped sampling the shadow map from passing
    # forever — `render.golden`'s third case makes the same argument about the first-light shadow.
    cy_add_test(
        NAME virtual_geometry_shaded
        KIND render
        SOURCES
            "${cy_render_tests_dir}/golden.cpp"
            "${cy_render_tests_dir}/test_virtual_geometry_shaded.cpp"
        DEPENDENCIES cy::sample-fidelity-frame cy::rendering-shadows cy::rendering-virtual-geometry
                     cy::rhi cy::rhi-null cy::rhi-vulkan
        # The reference lives beside its test, as `render.golden`'s does, and CTest runs a suite
        # from its BINARY directory.
        DEFINITIONS CY_RENDER_TEST_DIR="${cy_render_tests_dir}")
    # For "device.h" and "golden.h"; "frame.h", "scene.h" and "shade.h" arrive on the sample
    # library's own public include directory.
    target_include_directories(cy_test_render_virtual_geometry_shaded PRIVATE "${cy_render_tests_dir}")

    # MSAA AND MULTI-VIEW THROUGH THE RENDER GRAPH, ON A DEVICE. M11.d tasks 5.1 and 5.2. The graph's
    # own suites prove the twin, the resolve's placement, the refusals and the capability-selected
    # recording without a GPU; what they cannot show is that a resolve resolved anything, or that
    # the per-view baseline put each view in its own layer. This suite reads the pixels: 4x edges
    # against 1x, 1x declared through the model against 1x declared the old way, and a device with
    # multi-view against one created without it. KIND render because it needs a GPU.
    cy_add_test(
        NAME msaa_multiview
        KIND render
        SOURCES "${cy_render_tests_dir}/test_msaa_multiview.cpp"
        DEPENDENCIES cy::rendering-graph cy::rhi cy::rhi-null cy::rhi-vulkan)
    # For "device.h" and "shaders/view_probe_spirv.h", as the suites above.
    target_include_directories(cy_test_render_msaa_multiview PRIVATE "${cy_render_tests_dir}")

    cy_add_test(
        NAME environment_field
        KIND render
        SOURCES "${cy_render_tests_dir}/test_field_device.cpp"
        DEPENDENCIES cy::environment cy::rhi cy::rhi-null cy::rhi-vulkan cy::world)
    # For "device.h" and "shaders/field_probe_spirv.h", as the conventions suite above.
    target_include_directories(cy_test_render_environment_field PRIVATE "${cy_render_tests_dir}")

    # CLOUD SHADOWS IN THE WORLD FRAME. `atmosphere-sky-and-clouds` — "Cloud shadows". The frame
    # that consumes the field is `samples/10-world`'s, so this suite draws with THAT pipeline's
    # committed SPIR-V over ground lit by a real `sky::CloudShadowField`, and reads the pixels back:
    # darker under the cloud, unchanged beside it, the same frame with the field off or the sky
    # clear, and the sky and the ambient term untouched. It includes the sample's shader header by
    # path rather than linking the sample — the sample is an executable and its shaders are the
    # only part of it under test.
    cy_add_test(
        NAME world_cloud_shadow
        KIND render
        SOURCES "${cy_render_tests_dir}/test_world_cloud_shadow.cpp"
        DEPENDENCIES cy::environment cy::rendering-graph cy::rendering-sky cy::rhi cy::rhi-null
                     cy::rhi-vulkan cy::world)
    target_include_directories(cy_test_render_world_cloud_shadow
                               PRIVATE "${cy_render_tests_dir}" "${CMAKE_SOURCE_DIR}/samples")

    # WATER SHADING IN THE WORLD FRAME. `water` — "Water surface shading" and "Caustics". Drawn with
    # `samples/10-world`'s committed world and water SPIR-V and with the sample's own water device
    # half (`water_surface.cpp`, compiled in rather than linked, as the shader headers are included):
    # the refraction and mirrored reflection pictures, then the frame that samples them. Each water
    # pixel is checked against src/water/'s absorption law, Fresnel times the mirrored sky, the
    # shoreline band and the surface's own caustic focus; with water shading off the frame is
    # byte-identical to `references/world_water_off.png`.
    cy_add_test(
        NAME world_water
        KIND render
        SOURCES
            "${cy_render_tests_dir}/golden.cpp"
            "${cy_render_tests_dir}/test_world_water.cpp"
            "${CMAKE_SOURCE_DIR}/samples/10-world/water_surface.cpp"
        DEPENDENCIES cy::rendering-graph cy::rhi cy::rhi-null cy::rhi-vulkan cy::water cy::world
        DEFINITIONS CY_RENDER_TEST_DIR="${cy_render_tests_dir}")
    target_include_directories(cy_test_render_world_water
                               PRIVATE "${cy_render_tests_dir}" "${CMAKE_SOURCE_DIR}/samples")
endfunction()

cy_render_declare_tests()
