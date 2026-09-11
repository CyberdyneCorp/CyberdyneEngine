# samples/08-vertical-slice — the M8.b milestone artefact. Section 12.
#
# AN EIGHTH SHAPE, and it is the shape of the milestone: one program that is a GAME, and one driver
# that measures it.
#
#   cy_sample_vertical-slice  the game. A level, characters that animate and think, abilities with
#                             effects, a heads-up interface, sound and a 2D menu — every one of them
#                             the engine's real module, driven in one tick, with the frame assembled
#                             out of the renderer's own parts at the end of it.
#   slice.py                  the driver: four acts, every claim the program printed checked, and
#                             the picture drawn from the frame's own draw list. It reports through
#                             samples/harness/artefact.py, which is what makes a recorded gap a
#                             non-zero exit and refuses an extreme value as a headline.
#
# `just run-vertical-slice` is the recipe. `just run-sample vertical-slice` runs the program alone.
#
# ================================================================================================
# WHY THIS TARGET'S DEPENDENCY LIST IS THE POINT
# ================================================================================================
#
# M7's closing gate found eight modules under src/rendering/ "linked by nothing but their own test
# binaries", and M8.b's section 11 answered it with cy::rendering-assembly — which was then itself
# linked by nothing but its own two test binaries. This line is what removes that sentence from the
# tree: the assembly is linked by a program that plays a game with it.
#
# The same argument applies to cy::animation, cy::ai, cy::navigation, cy::ui, cy::audio-acoustics,
# cy::camera, cy::rendering-2d, cy::gameplay-abilities and cy::graph. Before this directory each of
# them was reachable only from its own suites; every one of them is now called from one loop, in one
# order, by one program. A capability at Working that no artefact uses is a capability whose
# integration nobody has tried.
#
# LAYER tools, like every sample: it consumes the whole engine and nothing consumes it.
#
# ================================================================================================
# AND WHY IT DECLARES NOTHING AT ALL WHEN A FEATURE IT NEEDS IS OFF
# ================================================================================================
#
# CY_ANIMATION and CY_AI are options `animation-and-skinning` and `ai-system` require their runtimes
# to be removable by, and `-D CY_ANIMATION=OFF` removes src/animation/ from the build entirely — the
# target `cy::animation` then does not exist. A sample that named it anyway would turn "this feature
# is removable" into a CONFIGURE ERROR naming the sample, which is the shape M8.b's own spike found
# in `-D CY_RENDERER_VULKAN=OFF`: an option declared, defaulted on, and never once built without.
#
# So the artefact is declared only where its subjects are, exactly as samples/04-character declares
# nothing without a Swift toolchain and samples/06-open-world nothing without the build tools. What
# that costs is stated rather than hidden: with either option off there is no vertical slice and no
# `smoke.vertical_slice`, and `just run-vertical-slice` reports an absent binary rather than a
# defect. The milestone's ledger runs the artefact in the default configuration, where both are ON.

if(NOT CY_ANIMATION OR NOT CY_AI OR NOT CY_UI OR NOT CY_VFX)
    message(STATUS "samples/08-vertical-slice: CY_ANIMATION, CY_AI, CY_UI or CY_VFX is off, so the "
                   "vertical slice — a game that animates, thinks, puts an interface over both and "
                   "throws sparks while it does — is not declared.")
    return()
endif()

# ================================================================================================
# WHAT M8.c ADDS, AND WHERE EACH PIECE COMES FROM
# ================================================================================================
#
#   spectacle.cpp   the particles and the cut, attached at the two seams M8.b's README recorded:
#                   `ActivationPipeline::cues()` and the camera. It writes no gameplay state and no
#                   camera transform, which is tasks 5.2 and 5.3.
#   capture.cpp     the frame RECORDED on a graphics device rather than assembled and dropped —
#                   `cy::rendering-pipeline`'s record callbacks, `cy::rendering-particles`'
#                   extension, and the PNG the milestone commits. Compiled always; it reports an
#                   absent device rather than failing, so the artefact still runs on a machine with
#                   no GPU.
#   tests/render/golden.cpp   THE ENGINE'S OWN PNG ENCODER, the one tests/render/ and
#                   src/rendering/pipeline/tests/ already write their captures with. A second
#                   encoder here would mean two writers and one reader for the committed images.
#
# AND THE EFFECT IS `src/vfx/tests/effects.cpp`, COMPILED IN RATHER THAN COPIED. That file authors
# the spark plume with CyberGraph and cooks it through `compile_system`; `integration.vfx_compiler`
# asserts what it compiles to, `integration.vfx` what it simulates to and `render.vfx` what it draws
# as. Writing a second plume in this directory would be a fourth description of one effect drifting
# from the other three inside a milestone — the argument tests/render/README.md makes about golden
# scenes. What the artefact adds is the one thing those three suites cannot say: that it survives
# being played from a gameplay cue, inside a running game, at the population a fight produces.

cy_add_module(
    NAME cy_sample_vertical-slice
    LAYER tools
    TYPE EXECUTABLE
    SOURCES
        main.cpp
        slice.cpp
        systems.cpp
        tick.cpp
        shot.cpp
        presentation.cpp
        spectacle.cpp
        capture.cpp
        "${CMAKE_SOURCE_DIR}/src/vfx/tests/effects.cpp"
        "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
    PRIVATE_DEPENDENCIES
        cy::graph
        cy::gameplay
        cy::gameplay-abilities
        cy::animation
        cy::ai
        cy::navigation
        cy::ui
        cy::text-unicode
        cy::audio-acoustics
        cy::camera
        cy::rendering-2d
        cy::rendering-assembly
        cy::rendering-particles
        cy::rendering-pipeline
        cy::rendering-scene
        cy::scene
        cy::sequencing
        cy::sequencing-camera
        cy::servers-camera
        cy::vfx
        cy::rhi
        cy::ecs)

target_include_directories(cy_sample_vertical-slice
                           PRIVATE "${CMAKE_SOURCE_DIR}/src/vfx/tests"
                                   "${CMAKE_SOURCE_DIR}/tests/render")

# THE VULKAN BACKEND IS A LINK-TIME CHOICE AND NOT A COMPILE-TIME ONE. `capture.cpp` asks
# `cy::rhi` for a device and reports what it got; with `-D CY_RENDERER_VULKAN=OFF` there is no
# device to get and `--capture` says so and exits non-zero, which is a reported gap rather than a
# configure error naming the artefact. See samples/03-first-light for the same arrangement.
if(CY_RENDERER_VULKAN)
    target_link_libraries(cy_sample_vertical-slice PRIVATE cy::rhi-vulkan)
endif()

if(NOT CY_BUILD_TESTS)
    return()
endif()

find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "samples/08-vertical-slice: python3 is absent, so smoke.vertical_slice is not "
                   "declared.")
    return()
endif()

get_directory_property(CY_SLICE_TIMEOUT DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                       DEFINITION CY_TEST_TIMEOUT_smoke)
if(NOT CY_SLICE_TIMEOUT)
    message(FATAL_ERROR
        "samples/08-vertical-slice: the test taxonomy's timeouts could not be read from "
        "tests/CMakeLists.txt. If those variables were renamed, this lookup should move with them.")
endif()

# THE SMOKE ENTRY RUNS THE WHOLE ARTEFACT on a smaller squad: `--agents` is the one dial whose cost
# is linear in the thing the exit criterion measures, so a smaller run checks every claim's SHAPE
# and the scale act is what checks its size — at 2,000 here rather than at the criterion's 8,000,
# which `just run-vertical-slice` is for. `--sample` hands the driver the program this configuration
# just built, so nothing here builds anything.
#
# It needs no display and no graphics device: the frame is assembled and no pass is recorded, which
# is what makes the artefact judgeable on a hosted runner.
add_test(NAME smoke.vertical_slice
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/slice.py"
                 --sample "$<TARGET_FILE:cy_sample_vertical-slice>"
                 --work "${CMAKE_CURRENT_BINARY_DIR}/run"
                 --agents 192
                 --ticks 120
                 --scale-agents 2000
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(smoke.vertical_slice PROPERTIES
                     LABELS "smoke" TIMEOUT "${CY_SLICE_TIMEOUT}")
