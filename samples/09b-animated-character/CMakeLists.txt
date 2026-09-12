# samples/09b-animated-character — M8.d's artefact. The import-to-pixels chain, end to end.
#
# A TENTH SHAPE, and the shape is the milestone's own claim: one program that takes four files an
# artist exported and ends with a character deforming on a screen. Every earlier sample renders
# geometry it generated itself — samples/07-fidelity generates its shells, samples/08-vertical-slice
# draws one procedural box per mesh asset — so before this directory NOTHING in the repository had
# ever drawn an imported mesh at all.
#
#   cy_sample_animated-character  the program: import, retarget, compile, animate, skin, draw, and a
#                                 numbered PNG per frame.
#   just capture-animated-character  the recipe that runs it and turns the sequence into an mp4.
#
# ================================================================================================
# WHY THIS TARGET'S DEPENDENCY LIST IS THE POINT, AGAIN
# ================================================================================================
#
# M8.d landed four bodies of work that had never met: `tools/import/`'s steps 7 and 8, the retarget
# correspondence in `src/animation/retarget_build.h`, the locomotion compiler in
# `src/graph/locomotion.h`, and the skinning dispatch in `src/rendering/skinning/`. Each was reached
# only from its own suite. This line is what removes that sentence from the tree: one target names
# all four, in one translation unit each, and the seams between them either line up or this program
# does not run.
#
# IT LINKS A TOOL, which only samples/06-open-world has done before. `cy::import` is LAYER tools and
# so is every sample, so the link is legal — and it is the only way to demonstrate an IMPORT: an
# artefact that read a pre-cooked asset out of the tree would be demonstrating the loader, and there
# is no loader for a cooked skeleton or clip yet.
#
# ================================================================================================
# AND WHY IT DECLARES NOTHING WHEN A FEATURE IT NEEDS IS OFF
# ================================================================================================
#
# `-D CY_ANIMATION=OFF` removes src/animation/ entirely and the target `cy::animation` then does not
# exist; a sample that named it anyway would turn "this feature is removable" into a CONFIGURE ERROR
# naming the sample. samples/08-vertical-slice's CMakeLists.txt argues that at length and this file
# only obeys it. The Vulkan backend is a LINK-time choice rather than a compile-time one, exactly as
# samples/03-first-light and samples/07-fidelity arrange it: the program builds without it and says
# no device answered.

if(NOT CY_ANIMATION)
    message(STATUS "samples/09b-animated-character: CY_ANIMATION is off, so the artefact that "
                   "imports a skeleton, retargets three clips onto it and skins a character with "
                   "the result is not declared.")
    return()
endif()

# ================================================================================================
# WHAT IS COMPILED IN, AND WHY IT IS NOT LINKED
# ================================================================================================
#
# tests/render/golden.cpp is THE ENGINE'S OWN PNG ENCODER — the one tests/render/,
# src/rendering/pipeline/tests/ and samples/08-vertical-slice already write their captures with. A
# second encoder here would mean two writers and one reader for the committed images, which is the
# argument every other call site of this pattern makes. It is compiled in rather than linked because
# it belongs to a test directory that declares test targets, not a library.
#
# shaders/character_spirv.h is CHECKED IN, compiled offline by shaders/embed_spirv.py, for the reason
# src/rendering/skinning/shaders/ gives: `CY_SHADER_SLANG` is off in Profile and Shipping, and an
# artefact that needed a shader compiler would not run on the machines that most need to run it.

cy_add_module(
    NAME cy_sample_animated-character
    LAYER tools
    TYPE EXECUTABLE
    SOURCES
        main.cpp
        character.cpp
        stage.cpp
        "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
    PRIVATE_DEPENDENCIES
        cy::animation
        cy::graph
        cy::import
        cy::rendering-graph
        cy::rendering-skinning
        cy::servers-render-geometry
        cy::rhi
        cy::rhi-null)

target_include_directories(cy_sample_animated-character
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                   "${CMAKE_SOURCE_DIR}/tests/render")

# THE VULKAN BACKEND IS A LINK-TIME CHOICE AND NOT A COMPILE-TIME ONE, exactly as
# samples/03-first-light, 07-fidelity and 08-vertical-slice arrange it: with
# `-D CY_RENDERER_VULKAN=OFF` the program still builds, asks `cy::rhi` for a device, and reports that
# none answered — a stated gap rather than a configure error naming the artefact. The definition is
# the sample's own rather than the option's name, because a translation unit that tested the option
# directly would be testing a variable no target hands it.
if(CY_RENDERER_VULKAN)
    target_link_libraries(cy_sample_animated-character PRIVATE cy::rhi-vulkan)
    target_compile_definitions(cy_sample_animated-character PRIVATE CY_SAMPLE_CHARACTER_VULKAN=1)
endif()

# ================================================================================================
# THERE IS NO CTest ENTRY HERE, AND THAT IS DELIBERATE
# ================================================================================================
#
# The program needs a Vulkan device AND four 1–17 MB Mixamo exports that are not in the repository
# and are not redistributable. just/run.just:366's own note about `capture-virtual-geometry` states
# the rule this follows: "It needs a graphics device. On a machine without one the tool says so and
# writes nothing, which is why this is a recipe a person runs and not a test." Adding a suite that
# skipped on every machine without both would be a suite whose failure nobody would notice.
#
# What IS gated automatically is everything underneath it: `unit.animation`, `integration.animation_*`,
# `integration.graph_compiler`, `unit.import`, `render.skinning` and `render.skinned_draw` each test
# one link of the chain this artefact walks end to end.
