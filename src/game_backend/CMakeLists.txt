# src/game_backend/ — the adapters behind ABI 1.3's game services. `add-swift-game-api`.
#
# `cy::abi` owns the entries and their thunks and names no server (cy/abi/game/services.h). This
# module is the other half: one adapter per service, each implementing its `cy::abi::game::*Backend`
# over the engine subsystem that owns the verb, and each binding itself onto a `cy::abi::Host` with
# its own `bind()`. It sits beside src/editor_backend/ and for the same reason: it is above the ABI
# and above the subsystems it adapts, and nothing below it links back.
#
# ONE SOURCE FILE PER SERVICE, OWNED BY ONE IMPLEMENTER (design.md, "File ownership"):
#   input_backend.cpp, camera_backend.cpp        A
#   physics_backend.cpp, navigation_backend.cpp  B
#   audio_backend.cpp, spawn_backend.cpp         C
#
# AND ONE THAT IS NOT AN ADAPTER: graph_behaviours.cpp (#29, visual scripting) runs compiled gameplay
# graphs on scene entities, and plays their cues through the audio adapter above — the same verb a
# Swift game reaches through the ABI — so it lives beside the adapters it shares.
cy_add_module(
    NAME cy_game_backend
    LAYER abi
    ALIAS cy::game-backend
    TYPE STATIC
    SOURCES
        src/input_backend.cpp
        src/camera_backend.cpp
        src/physics_backend.cpp
        src/navigation_backend.cpp
        src/audio_backend.cpp
        src/spawn_backend.cpp
        src/graph_behaviours.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES
        cy::abi
        cy::graph
        cy::servers-input
        cy::servers-camera
        cy::camera
        cy::servers-physics
        cy::navigation
        cy::servers-audio
        cy::scene
        cy::scene-serialization)

# One integration suite per service, each registered when its source exists — so an implementer adds
# a test file and nothing else, and a service with no suite is visibly absent at configure time.
if(CY_BUILD_TESTS)
    set_property(GLOBAL PROPERTY CY_GAME_BACKEND_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    function(cy_game_backend_declare_tests)
        get_property(tests_dir GLOBAL PROPERTY CY_GAME_BACKEND_TESTS_DIR)
        get_directory_property(budget DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_BUDGET_integration)
        get_directory_property(timeout DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
                               DEFINITION CY_TEST_TIMEOUT_integration)
        set(CY_TEST_BUDGET_integration "${budget}")
        set(CY_TEST_TIMEOUT_integration "${timeout}")
        # A suite that needs a concrete backend behind its server's interface names it here; the
        # adapters themselves never link one.
        set(extra_physics cy::physics-reference)
        foreach(service IN ITEMS input camera physics navigation audio spawn graph)
            if(EXISTS "${tests_dir}/test_${service}_backend.cpp")
                cy_add_test(
                    NAME game_backend_${service}
                    KIND integration
                    SOURCES "${tests_dir}/test_${service}_backend.cpp"
                    DEPENDENCIES cy::game-backend ${extra_${service}})
            else()
                message(STATUS "game_backend: no suite for '${service}' yet "
                               "(tests/test_${service}_backend.cpp)")
            endif()
        endforeach()
        # The gameplay graph suite reads the graph the editor writes for #29's acceptance scenario
        # from the one place both sides compare against.
        if(TARGET cy_test_integration_game_backend_graph)
            target_compile_definitions(cy_test_integration_game_backend_graph PRIVATE
                "CY_SCRIPT_FIXTURE_DIR=\"${CMAKE_SOURCE_DIR}/src/editor_backend/tests/data\"")
        endif()
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_game_backend_declare_tests)
endif()
