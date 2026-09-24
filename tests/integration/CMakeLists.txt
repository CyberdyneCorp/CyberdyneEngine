# tests/integration/ — subsystems together, and anything that touches the world outside the process.
#
# A second per test case. Filesystem, several modules at once, a server with a null backend: work
# that is real enough to be worth a second and cheap enough to run before every push.
#
# The suites here are flat rather than one directory per module, because an integration test is
# about a seam between modules and has no single owner to file it under.

# The directory the seam test scans: tests/ itself, resolved once rather than spelled with a ".."
# that every reader has to re-resolve.
get_filename_component(cy_tests_dir "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)

# The gate's probe for the stall ceiling (M11.c's fifth close): cases that are meant to trip the
# budget guard, compiled with the unit tier's budget. No CTest entry runs it — every case in it
# fails by design — so it is a plain executable that the harness suite below runs as a child and
# whose verdicts it reads. See stall_probe.cpp.
cy_add_module(
    NAME    cy_test_stall_probe
    LAYER   tools
    TYPE    EXECUTABLE
    SOURCES stall_probe.cpp
    PRIVATE_DEFINITIONS CY_TEST_BUDGET_NS=${CY_TEST_BUDGET_unit}ULL
    PRIVATE_DEPENDENCIES cy::test-harness)

cy_add_test(
    NAME    harness
    KIND    integration
    SOURCES
        test_budget_contention.cpp
        test_temp_dir.cpp
        test_wrapper_seam.cpp
    # The seam test reads the test tree it is checking. The path is compiled in rather than
    # discovered, because a test that guesses where the sources are passes vacuously when it guesses
    # wrong. The probe's path is compiled in for the same reason.
    DEFINITIONS
        CY_TESTS_SOURCE_DIR="${cy_tests_dir}"
        "CY_STALL_PROBE=\"$<TARGET_FILE:cy_test_stall_probe>\"")
# The suite runs the probe by path, so it must exist first; nothing links it to order it.
add_dependencies(cy_test_integration_harness cy_test_stall_probe)

# The state hash's coverage over a scene world. Here rather than in a module because it is a claim
# about three at once — the ECS's built-in components, the scene's, and the runtime's walk over both
# — and because the thing it defends is a property of the composition: at M2's close the hash was
# silent about thirteen of seventeen subjects, and a suite inside any one module would have been
# able to say only that its own half was declared.
cy_add_test(
    NAME    state_hash_coverage
    KIND    integration
    SOURCES test_state_hash_coverage.cpp
    DEPENDENCIES cy::runtime cy::scene cy::ecs cy::core-determinism cy::core-values)

# The one registry every producing module declares into. Here rather than in any one module because
# the defect it exists for lives BETWEEN modules: `foliage` and `weather` each declared the standard
# `wind` field, differently, and `FieldRegistry::declare()` refuses the second in either order — so
# a project using both rows failed at startup while both modules' own suites stayed green. M10's
# gate found it; this is the check that would have. It links every row that declares a field, which
# is what makes it an integration suite and not a unit one.
cy_add_test(
    NAME    standard_fields
    KIND    integration
    SOURCES test_standard_fields.cpp
    DEPENDENCIES
        cy::environment
        cy::foliage
        cy::rendering-sky
        cy::terrain
        cy::water
        cy::weather)

# THE HIERARCHICAL DEPTH BUFFER ON THE DEVICE, JUDGED AGAINST THE CPU MODEL. M11.c task 4.1 and 4.2.
#
# Here rather than in `cy::rendering-hzb`'s own tests because the claim spans three modules and no
# one of them owns it: hzb/ builds the pyramid, `cy::servers-render-culling` is the CPU model that
# has passed its own tests since M6, and `cy::rendering-gpu-culling` is the consumer whose dispatch
# refused `kGpuCullOcclusion` by name until this rung. The requirement M11.c adds to
# `rendering-culling-and-lod` is exactly about that seam: a model passing its own tests is not a
# pass, and the device answer has to be compared against the model's for the same scene.
#
# It needs a Vulkan device and REPORTS "NOT EVALUATED" without one rather than passing on the
# model's behalf. The ledger criterion that runs it carries `requires = "gpu"` for the same reason.
#
# DECLARED ONLY WITH THE VULKAN BACKEND COMPILED IN, which is `tests/render/CMakeLists.txt`'s rule
# for the same reason and was missing here: it links `cy::rhi-vulkan`, that target does not exist
# under `-D CY_RENDERER_VULKAN=OFF`, and CMake's GENERATE step then failed for the whole tree rather
# than for this test — `m8c:feature-options-off` red with "Target cy_test_integration_rendering_culling
# links to cy::rhi-vulkan but the target was not found". Not declared rather than declared-and-skipped,
# because a suite that always skips is a suite whose failure nobody would notice.
if(CY_RENDERER_VULKAN)
    cy_add_test(
        NAME    rendering_culling
        KIND    integration
        SOURCES test_rendering_culling.cpp
        DEPENDENCIES
            cy::rendering-gpu-culling
            cy::rendering-graph
            cy::rendering-hzb
            cy::rhi
            cy::rhi-null
            cy::rhi-vulkan
            cy::servers-render-culling)
endif()

# The engine running frames on a platform that shares no desktop assumption. M11.d task 4.4.
#
# Here rather than under platform/stub/tests/ for a structural reason: the claim is about `Runtime`
# — a layer-5 target — standing on a layer-3 port, and a suite declared from platform/ may not
# include cy/runtime/runtime.h at all. tools/layercheck/ gives platform/ layer 3 and tests/ layer 7,
# so this is the only directory from which the seam can be tested at all.
#
# It is the porting surface's proof, and platform/linux-native/ is not: two desktop backends
# disagree about which library opens a window and agree about everything else a desktop takes for
# granted. See the file's own header.
cy_add_test(
    NAME    stub_platform
    KIND    integration
    SOURCES test_stub_frame.cpp
    DEPENDENCIES cy::runtime cy::platform-stub)
