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
endfunction()

cy_render_declare_tests()
