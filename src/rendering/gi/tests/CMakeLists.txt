# src/rendering/gi/tests/ — the illumination suites. Tasks 9.1, 9.2 and 9.5.
#
# Declared through the deferral src/rendering/culling/tests/ uses: cy_add_test() is defined by
# tests/CMakeLists.txt, which the top-level file adds after src/, so the declaration waits until the
# end of the top-level directory's processing and the taxonomy is read back out of tests/ rather
# than restated.
#
#   render_gi           the decisions that are arithmetic — detail selection, the exclusion mask,
#                       the far-field ramp, the reflection table, the probe encodings, the GI
#                       budget's control law and the GI volumes' resolution. Fits the unit budget
#                       because none of it traces.
#   render_gi_pipeline  the subsystems on a real room: the sparse field and its scroll, the surface
#                       cache's multi-bounce feedback, adaptive probe placement and the leak the
#                       visibility term stops, the tiers and their confidence, the reflection
#                       capture, the bake's seeding, and a frame of the composed system. Every case
#                       traces real rays through a real field; that is what an integration test is.
#                       Also the host half of the device surface cache (issue #35): the field's
#                       change journal, the card snapshot against the cache's own lookup, the shadow
#                       map against the field, and the shading seam `system.h` chooses CPU or GPU by.
#                       And a mesh's cards and distance grid as a cook builds them: brute force over
#                       every triangle, which is over the unit budget by design.
#   render_gi_signals   the five stochastic signals the denoiser declares, PRODUCED — indirect
#                       diffuse, indirect specular, ray-traced shadows, ambient occlusion and
#                       stochastic direct — and each routed through `denoise::Denoiser`. M11.c task
#                       2.5: until that rung, nothing in this tree called `denoise()` outside the
#                       denoiser's own suites.
#   render_gi_fallback  M7's own exit criterion: ray tracing disabled falls back to software tracing
#                       with no visual discontinuity beyond tolerance. Its own suite because it is a
#                       milestone criterion rather than a unit of behaviour, and because a criterion
#                       buried in a larger file is a criterion nobody runs on its own.
#   render_gi_volume    irradiance volumes: SH capture against a CPU reference integration, colour
#                       bleeding near a coloured wall and not far from it, an unlit wall lit by
#                       bounce, the visibility term, the update policy, and a capture through the
#                       composed system's own tracer and surface cache.
#   render_gi_emissive  emissive surfaces as illumination: classification, promotion, the form
#                       factor, and the path tracer as the reference for both paths.
#   render_gi_volumes   GI volumes through the composed system.
#   render_gi_screen    the per-pixel resolve and its denoiser (issue #35, stage 4): the host
#                       oracle the device transcribes, and the frame's quality bars against the
#                       path tracer, a 10 cm wall and a still view.
#   render_gi_screen_convergence  a light that moves, and the frames until the image is within a
#                       stated distance of the new light's converged image. SMOKE: it converges two
#                       scenes before it counts, ninety-one whole frames, over the integration budget.
#   render_gi_classes   illumination classes of moving objects: the derivation and override, a
#                       projectile that invalidates nothing, a vehicle whose invalidation is
#                       amortised, a character's proxy against the path tracer, and a crowd's
#                       coarse proxies.
#   render_gi_media     transparency and participating media: the staging table, thin
#                       transmission and the medium's indirect grid against the path tracer, and the
#                       deferred features refused by name.
#   render_gi_diagnostics  the point and region queries, the darkness causes, the leak suspects,
#                       and the per-frame profile with its attribution.
#   render_gi_debug_views  the nineteen GI debug views, each against a committed golden image
#                       (`references/`), compiled with tests/render/golden.cpp for the PNG codec.
set_property(GLOBAL PROPERTY CY_RENDER_GI_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_render_gi_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_RENDER_GI_TESTS_DIR)

    foreach(kind unit integration smoke)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/rendering/gi/tests: the '${kind}' entry of the test taxonomy could not be "
                "read from tests/CMakeLists.txt.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME render_gi
        KIND unit
        SOURCES
            "${tests_dir}/test_gi_volumes.cpp"
            "${tests_dir}/test_policy.cpp"
        DEPENDENCIES cy::rendering-gi)

    cy_add_test(
        NAME render_gi_pipeline
        KIND integration
        SOURCES
            "${tests_dir}/test_caches.cpp"
            "${tests_dir}/test_card_lighting.cpp"
            "${tests_dir}/test_probe_gather.cpp"
            "${tests_dir}/test_field.cpp"
            "${tests_dir}/test_mesh_cards.cpp"
            "${tests_dir}/test_system.cpp"
            "${tests_dir}/test_tiers.cpp"
            "${tests_dir}/test_window_light.cpp"
        DEPENDENCIES cy::rendering-gi)

    cy_add_test(
        NAME render_gi_fallback
        KIND integration
        SOURCES "${tests_dir}/test_fallback.cpp"
        DEPENDENCIES cy::rendering-gi)

    # THE FIVE STOCHASTIC SIGNALS, PRODUCED AND ROUTED. M11.c task 2.5. Its own suite for the reason
    # render_gi_fallback has one: it answers a milestone criterion rather than a unit of behaviour,
    # and a criterion buried in a larger file is a criterion nobody runs on its own. INTEGRATION
    # because every case traces real rays through a real field and puts real buffers through the
    # real denoiser — the whole point being that no buffer here was written by the test asserting on
    # it.
    cy_add_test(
        NAME render_gi_signals
        KIND integration
        SOURCES "${tests_dir}/test_signals.cpp"
        DEPENDENCIES cy::rendering-gi)

    cy_add_test(
        NAME render_gi_volume
        KIND integration
        SOURCES "${tests_dir}/test_irradiance_volume.cpp"
        DEPENDENCIES cy::rendering-gi)

    # EMISSIVE SURFACES AS ILLUMINATION: the promoted emitter's form factor against its closed
    # form and a quadrature, the classification and its energy accounting, a glowing panel and a
    # tiny lamp measured against the path tracer — promoted and left to the cache — and the
    # composed system's ingestion. Integration because the reference traces real paths.
    cy_add_test(
        NAME render_gi_emissive
        KIND integration
        SOURCES "${tests_dir}/test_emissive.cpp"
        DEPENDENCIES cy::rendering-gi)

    # THE PER-PIXEL RESOLVE AND ITS DENOISER, the host oracle of issue #35's stage 4, and the quality
    # bars the frame's GI is held to: ground truth over the exact geometry, a 10 cm wall, a light
    # that moves and a still view. Here rather than on the device because the device suite holds
    # the device to this oracle to a few parts in ten million, and this runs where there is none.
    # The light that moves is its own SMOKE suite: two converged scenes and the frames between them
    # cost a second of CPU on a hosted runner, which is the integration budget's whole.
    cy_add_test(
        NAME render_gi_screen
        KIND integration
        SOURCES "${tests_dir}/test_screen_resolve.cpp"
        DEPENDENCIES cy::rendering-gi)
    cy_add_test(
        NAME render_gi_screen_convergence
        KIND smoke
        SOURCES "${tests_dir}/test_screen_convergence.cpp"
        DEPENDENCIES cy::rendering-gi)

    # THE SCREEN-PROBE GATHER, the default final gather and the host oracle of its device passes:
    # adaptive placement at depth and normal steps and nowhere else, the probes' temporal
    # reprojection and blend, and the lever that keeps the per-pixel gather selectable. Its
    # quality on the guide's four axes beside the per-pixel gather converges a scene per gather and
    # per spacing, and is a smoke suite for that reason.
    cy_add_test(
        NAME render_gi_screen_probes
        KIND integration
        SOURCES "${tests_dir}/test_screen_probes.cpp"
        DEPENDENCIES cy::rendering-gi)
    cy_add_test(
        NAME render_gi_screen_probes_quality
        KIND smoke
        SOURCES "${tests_dir}/test_screen_probes_quality.cpp"
        DEPENDENCIES cy::rendering-gi)

    # GI VOLUMES THROUGH THE COMPOSED SYSTEM: an important interior's probes and updates within one
    # allocation, the region placed again when the volume goes, a scene with none bit for bit, the
    # intensity and far-field overrides, and the tier preference. Resolution alone is render_gi.
    cy_add_test(
        NAME render_gi_volumes
        KIND integration
        SOURCES "${tests_dir}/test_gi_volume_system.cpp"
        DEPENDENCIES cy::rendering-gi)

    # THE SETTINGS A GAME OR AN EDITOR CHANGES WHILE THE WORLD RUNS: mode, global quality and tier,
    # hardware ray tracing and the budget (`IlluminationSystem::set_runtime_settings`).
    cy_add_test(
        NAME render_gi_runtime
        KIND integration
        SOURCES "${tests_dir}/test_runtime_settings.cpp"
        DEPENDENCIES cy::rendering-gi)

    cy_add_test(
        NAME render_gi_classes
        KIND integration
        SOURCES "${tests_dir}/test_classes.cpp"
        DEPENDENCIES cy::rendering-gi)

    cy_add_test(
        NAME render_gi_media
        KIND integration
        SOURCES "${tests_dir}/test_media.cpp"
        DEPENDENCIES cy::rendering-gi)

    cy_add_test(
        NAME render_gi_diagnostics
        KIND integration
        SOURCES "${tests_dir}/test_diagnostics.cpp"
        DEPENDENCIES cy::rendering-gi)

    cy_add_test(
        NAME render_gi_debug_views
        KIND integration
        SOURCES
            "${tests_dir}/test_debug_views.cpp"
            "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
        DEPENDENCIES cy::rendering-gi cy::servers-render)
    target_include_directories(cy_test_integration_render_gi_debug_views
                               PRIVATE "${CMAKE_SOURCE_DIR}/tests/render")
    target_compile_definitions(cy_test_integration_render_gi_debug_views
                               PRIVATE CY_GI_TEST_DIR="${tests_dir}")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_render_gi_declare_tests)
