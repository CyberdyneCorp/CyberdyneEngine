# src/core/determinism/ — layer 0: the simulation clock, epochs, the commit boundary, seeded random
# streams, state classification, hierarchical state hashing, and stable tie-breaking.
#
# Tasks 4.2.1 through 4.2.7. Governed by `simulation-and-determinism`, which reaches **Seed** at M2:
# design.md §5 — "the shape is right and the hooks exist, not that anything is validated". The
# determinism profiles, the validator, the chaos scheduler, the lint and the divergence capture are
# M9's and are deliberately absent; what is here is what everything M3 through M8 will be written
# against, which is why it lands before there is anything to validate.
#
# WHY LAYER 0 AND NOT BESIDE THE RUNTIME. Every one of these is vocabulary a subsystem needs in its
# own headers: a physics component holding a `Classified<>` field, an AI system taking a
# `RandomStream` by value, a server declaring a `StateProvider`. Putting them at layer 5 would mean
# the ECS could not name them, and a classification the ECS cannot name is a classification that
# gets restated. The consequence is that nothing here knows what a component or an entity is; the
# walk that binds the hash to an `ecs::World` lives in src/runtime/state_hash.h, at layer 5, where
# it can.
#
# THE DEPENDENCIES ARE THE DESIGN. core-base for Expected and the types; core-memory for Array and
# the allocator, because a subsystem that grew its own buffer would be a second allocator outside
# the budget tree (design.md §1's argument, applied here); core-reflect because a state schema
# derives a field's class from its `PersistenceKind` and reads `FieldKind` to hash a value rather
# than its bytes. There is deliberately no dependency on core-diagnostics: a component header that
# declares one classified field would otherwise drag the trace in behind it.
#
# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.

cy_add_module(
    NAME cy_core_determinism
    LAYER core
    ALIAS cy::core-determinism
    TYPE STATIC
    SOURCES
        src/clock.cpp
        src/commit.cpp
        src/hash.cpp
        src/provider.cpp
        src/state_schema.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::core-reflect)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
