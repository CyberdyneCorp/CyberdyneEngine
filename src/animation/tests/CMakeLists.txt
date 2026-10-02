# src/animation/tests/ — CyberAnimation's suites. M8.b section 5.
#
# Declared with cy_add_test(), which tests/CMakeLists.txt defines — and the top-level CMakeLists.txt
# adds tests/ *after* src/, so that function does not exist while this file is read. The declaration
# is deferred to the end of the top-level directory's processing and the taxonomy is read back out
# of tests/ rather than restated, the same way src/graph/tests/ and src/physics/tests/ do it.
#
#   animation           unit. The asset model and the algebra over it: a skeleton's parent-before-
#                       child order and its bone levels of detail, a clip's error-bounded
#                       compression and its cursored sampling, the level-of-detail policy and its
#                       hysteresis, the pose cache, constraint conflict detection, the two-bone
#                       solver and retargeting.
#                       Every case builds a skeleton of a dozen joints and a clip of a few hundred
#                       keys, touches no file, no device and no world, and finishes well inside a
#                       millisecond.
#
#   animation_runtime   INTEGRATION, and deliberately. A case here compiles a graph through
#                       `cy::graph::pose::compile_pose`, binds a rig, advances a state machine over
#                       hundreds of ticks and evaluates hundreds of instances — the same shape
#                       src/graph/tests/ put its lowering cases in the integration tier for. The
#                       subject IS the repeated evaluation: making it cheap enough for the unit
#                       tier's millisecond would mean measuring something else. The retarget BAKE is
#                       here for the same reason and was moved here after being measured: at half
#                       the unit budget it spent 0.73 ms of 0.50 ms, and it is a cook. M8.d's
#                       crossing of three separately exported rigs onto one character is the same
#                       work three more times over a twenty-eight joint skeleton, so it is here and
#                       not beside the correspondence's own unit cases.
#
#   animation_system    integration. The per-frame system (src/animation/system/) over a real
#                       `runtime::Simulation`: an entity's machine against the hand-driven path bit
#                       for bit, level of detail, events, root motion, removal, five hundred
#                       instances over three rigs on a job system, and two runs that must agree.
#
#   animation_assets    integration. Issue #76 stage 2: the cooked skeleton, clip and program records
#                       round-trip byte for byte, and the library loads them through a real asset
#                       system, binds rigs by asset id and name, refuses what does not match, and
#                       swaps a reloaded clip under live instances.
#
#   animation_runtime_only
#                       unit, and a LINK-time test: it defines `compile_pose` itself, so it links
#                       only while a runtime that loads programs pulls in no compiler.
#
#   animation_teardown  integration, and hard rule 4's case. Pose worlds and batches created and
#                       destroyed with work in flight and four spinner threads holding the cores. A
#                       millisecond budget would be met by trimming the rounds until the race
#                       stopped reproducing, which is the opposite of the point.
set_property(GLOBAL PROPERTY CY_ANIMATION_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(cy_animation_declare_tests)
    get_property(tests_dir GLOBAL PROPERTY CY_ANIMATION_TESTS_DIR)

    foreach(kind IN ITEMS unit integration)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_${kind})
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_${kind})
        if(NOT budget OR NOT timeout)
            message(FATAL_ERROR
                "src/animation/tests: the '${kind}' entry of the test taxonomy could not be read "
                "from tests/CMakeLists.txt. If the budget variables were renamed, this lookup and "
                "the deferral around it should go away — declare the suites from tests/unit/ "
                "instead.")
        endif()
        set(CY_TEST_BUDGET_${kind} "${budget}")
        set(CY_TEST_TIMEOUT_${kind} "${timeout}")
    endforeach()

    cy_add_test(
        NAME animation
        KIND unit
        SOURCES
            "${tests_dir}/test_skeleton.cpp"
            "${tests_dir}/test_clip.cpp"
            "${tests_dir}/test_ik.cpp"
            "${tests_dir}/test_lod.cpp"
            "${tests_dir}/test_retarget.cpp"
            "${tests_dir}/test_retarget_build.cpp"
        DEPENDENCIES cy::animation)

    # The pose world's handoff is asserted against the module that consumes it rather than against a
    # copy of its rules: `cy::servers-render-geometry` is linked here so the case builds a real
    # `SkinningDescriptor` over a real `PoseWorld` offset and validates it.
    cy_add_test(
        NAME animation_runtime
        KIND integration
        SOURCES
            "${tests_dir}/test_bake.cpp"
            "${tests_dir}/test_clocks.cpp"
            "${tests_dir}/test_retarget_exports.cpp"
            "${tests_dir}/test_evaluate.cpp"
            "${tests_dir}/test_pose_world.cpp"
        DEPENDENCIES cy::animation cy::servers-render-geometry)

    # The per-frame system over a real `runtime::Simulation`: the tick half in the fixed step, the
    # pose half in `Stage::Animation`, and a job system for the cases that spread a batch. Issue #76
    # stage 1. Integration for the reason `animation_runtime` is: the subject is hundreds of ticks.
    cy_add_test(
        NAME animation_system
        KIND integration
        SOURCES "${tests_dir}/test_system.cpp"
        DEPENDENCIES cy::animation-system cy::core-jobs cy::runtime cy::scene)

    # The cooked records and the library over a real asset system. Issue #76 stage 2.
    cy_add_test(
        NAME animation_assets
        KIND integration
        SOURCES "${tests_dir}/test_assets.cpp"
        DEPENDENCIES cy::animation-assets cy::core-jobs)

    # A LINK-TIME TEST. runtime_only_probe.cpp defines `cy::graph::pose::compile_pose` itself, so
    # this target links only while nothing it depends on pulls the real compiler in. Its one case
    # then loads and plays a cooked program. The dependency list is deliberately the runtime and
    # the decoders and nothing that compiles.
    cy_add_test(
        NAME animation_runtime_only
        KIND unit
        SOURCES "${tests_dir}/runtime_only_probe.cpp"
        DEPENDENCIES cy::animation-assets)

    cy_add_test(
        NAME animation_teardown
        KIND integration
        SOURCES "${tests_dir}/test_teardown.cpp"
        DEPENDENCIES cy::animation)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_animation_declare_tests)
