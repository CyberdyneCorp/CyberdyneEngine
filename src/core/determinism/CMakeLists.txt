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

# The configuration-time half of the determinism profile (M9 task 2.2). Included here rather than in
# cmake/ because it is this module's own mechanism, and defined before any module that declares a
# profile is processed: src/CMakeLists.txt adds core/determinism at line 14 and gameplay at 94.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/determinism_profile.cmake")

cy_add_module(
    NAME cy_core_determinism
    LAYER core
    ALIAS cy::core-determinism
    TYPE STATIC
    SOURCES
        src/clock.cpp
        src/codec.cpp
        src/commit.cpp
        src/fp_policy.cpp
        src/hash.cpp
        src/profile.cpp
        src/provider.cpp
        src/state_schema.cpp
        src/validator.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::core-reflect
    # core-values for cy::Name: a state field may be declared as an interned name,
    # and what is hashed is its text rather than its index (StateEncoding).
    PRIVATE_DEPENDENCIES cy::core-values)

# THE FLAG IS LOAD-BEARING AND IT IS ADDED HERE, NOT BY THE FUNCTION THAT CHECKS IT.
#
# `fp_policy.cpp`'s twelve replacements are built out of the exact IEEE-754 set and the
# correctly-rounded set and out of nothing else, which is what makes their value the same whether
# the compiler folds the expression or leaves it to run time. Contraction would break that: `a*b + c`
# fused into one rounding is a different number from `a*b` rounded and then added, and M9's spike
# measured 231 of 267 values moving in the `state_hash` workload when the compiler was free to
# choose. PRIVATE, because it is a statement about how *these* translation units are compiled.
#
# Deleting this line makes `cy_declare_determinism_profile()` below fail the configure. That is the
# mutation that proves the check is a check; src/core/determinism/README.md records the run.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cy_core_determinism PRIVATE -ffp-contract=off)
elseif(MSVC)
    target_compile_options(cy_core_determinism PRIVATE /fp:precise)
endif()

cy_declare_determinism_profile(cy_core_determinism
    PROFILE SamePlatform
    REASON "its arithmetic is the exact IEEE-754 set and the correctly-rounded set measured by M9's spike; CrossPlatform would need deterministic math types, which this tree does not have")

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
