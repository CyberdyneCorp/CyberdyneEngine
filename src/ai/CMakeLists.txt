# src/ai/ — CyberAI: agents as entities, batched perception, the knowledge store, environment
# queries, smart objects, AI level of detail and the deterministic think scheduler.
# M8.b section 6, tasks 6.3 and 6.4.
#
# ================================================================================================
# WHAT IS HERE AND WHAT IS DELIBERATELY NOT
# ================================================================================================
#
# `ai-system` draws the line and this file keeps it: "The AI system SHALL be engine code: the agent
# model, the graph compiler, perception scheduling, the knowledge store, environment queries, smart
# objects, AI LOD, and the scheduler." All of that is here — except the GRAPH COMPILER, which is
# `cy::graph::behaviour` and belongs to `visual-scripting`, the same line src/animation/ draws
# between a runtime and a compiler. The compiled `BehaviourProgram` arrives here immutable and is
# shared by every agent running it.
#
# `ai-system`'s "No AI framework dependency" scenario: "WHEN the dependency manifest is audited THEN
# it SHALL contain no third-party game-AI framework". This target links cy::core, cy::ecs,
# cy::graph and cy::navigation, and deps/manifest.toml carries no AI library. The one third-party
# dependency anywhere near this stack is Recast, which is a navmesh GENERATOR and is declared as one.
#
# GATED BY CY_AI, which is ON by default (cmake/features.cmake says why). `ai-system`: "The system
# SHALL be removable at build time via `CY_AI`", and its scenario adds "navigation SHALL remain
# fully functional for non-AI users" — which is why the option gates this directory alone and
# src/navigation/ is built either way.
#
# LAYER. `scene` (4), the rung src/navigation/, src/graph/, src/world/ and src/gameplay/ all chose.
# The dependency list is what makes it honest: cy::ecs for the components, cy::graph for the
# program, cy::navigation for the movement `ai-system` requires agents to drive through it — and
# nothing above. No renderer, no device, no server.
cy_add_module(
    NAME cy_ai
    LAYER scene
    ALIAS cy::ai
    TYPE STATIC
    SOURCES
        src/agent.cpp
        src/environment_query.cpp
        src/knowledge.cpp
        src/locomotion.cpp
        src/perception.cpp
        src/runtime.cpp
        src/smart_object.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES
        cy::core-base
        cy::core-math
        cy::core-memory
        cy::core-values
        cy::ecs
        cy::graph
        cy::navigation)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
