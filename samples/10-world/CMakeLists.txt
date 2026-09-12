# samples/10-world — M10's artefact. Seven modules, one world, one video.
#
# AN ELEVENTH SHAPE, and the shape is the milestone's own claim: one program in which the seven
# things M10 built are wired to each other rather than each to its own suite.
#
#   cy_sample_world         the program: generate, cook, claim, place, simulate, draw, and a
#                           numbered PNG per frame.
#   just capture-world      the recipe that runs it and turns the sequence into an mp4.
#
# ================================================================================================
# WHY THIS TARGET'S DEPENDENCY LIST IS THE POINT
# ================================================================================================
#
# M10 landed `src/environment/`, `src/pcg/`, `src/terrain/`, `src/water/`, `src/foliage/`,
# `src/weather/` and the atmosphere half of `src/rendering/sky/`, and every one of them was reached
# only from its own tests. This line is what removes that sentence from the tree: one target names
# all seven, and the seams between them either line up or this program does not run.
#
# It also names what the modules themselves deliberately do NOT name. `cy::weather` does not link
# `cy::water`; `cy::water` does not link `cy::terrain`; `cy::pcg` links neither `cy::foliage` nor
# `cy::ecs`; `cy::rendering-sky` is not linked by any of them. Those absences are requirements made
# checkable by the link graph, and the wiring has to happen SOMEWHERE — an artefact is where.
#
# ================================================================================================
# THE VULKAN BACKEND IS A LINK-TIME CHOICE AND NOT A COMPILE-TIME ONE
# ================================================================================================
#
# Exactly as samples/03-first-light, 07-fidelity, 08-vertical-slice and 09b-animated-character
# arrange it: with `-D CY_RENDERER_VULKAN=OFF` the program still builds, asks `cy::rhi` for a
# device, reports that none answered, and still runs the whole world headless — which is the mode
# `--headless` selects on purpose, because every producer in this artefact runs on the processor and
# only the picture needs a device.

cy_add_module(
    NAME cy_sample_world
    LAYER tools
    TYPE EXECUTABLE
    SOURCES
        main.cpp
        world.cpp
        stage.cpp
        "${CMAKE_SOURCE_DIR}/tests/render/golden.cpp"
    PRIVATE_DEPENDENCIES
        cy::environment
        cy::foliage
        cy::pcg
        cy::pcg-adapters
        cy::rendering-graph
        cy::rendering-sky
        cy::rhi
        cy::rhi-null
        cy::terrain
        cy::water
        cy::weather
        cy::world)

target_include_directories(cy_sample_world
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                   "${CMAKE_SOURCE_DIR}/tests/render")

if(CY_RENDERER_VULKAN)
    target_link_libraries(cy_sample_world PRIVATE cy::rhi-vulkan)
    target_compile_definitions(cy_sample_world PRIVATE CY_SAMPLE_WORLD_VULKAN=1)
endif()

# ================================================================================================
# THERE IS NO CTest ENTRY HERE, AND THAT IS DELIBERATE
# ================================================================================================
#
# The picture needs a graphics device, and just/run.just:366's note about `capture-virtual-geometry`
# states the rule this follows: "It needs a graphics device. On a machine without one the tool says
# so and writes nothing, which is why this is a recipe a person runs and not a test." A suite that
# skipped on every machine without one would be a suite whose failure nobody would notice.
#
# What IS gated automatically is everything underneath it: `unit.environment`,
# `integration.environment_gpu`, `unit.terrain`, `integration.terrain_*`, `unit.water`,
# `integration.water_*`, `unit.foliage`, `integration.foliage_*`, `unit.weather`,
# `integration.weather_*`, `unit.pcg`, `integration.pcg_*` and `unit.sky` each test one of the parts
# this artefact composes.
