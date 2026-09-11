# src/replay/ — CyberReplay: ONE record, five readers. M9 section 1.
#
# ==================================================================================================
# WHAT THIS MODULE IS FOR, IN ONE PARAGRAPH
# ==================================================================================================
#
# M9's subtitle is "one command log, read five ways", and the failure it guards against is that
# replay grows a record, replication grows a second, the crash buffer grows a third, and the three
# drift until a replay of a networked session is a different program from the session.
# `replay-and-rollback`: "A second representation of participant intent SHALL NOT exist, because two
# representations of the same thing drift." So `include/cy/replay/record.h` holds `LogRecord`, every
# reader declares `using record_type = LogRecord;`, and `tests/test_one_record.cpp` fails by name
# when one does not.
#
# ==================================================================================================
# LAYER, AND WHY REPLICATION'S CURSOR LIVES HERE
# ==================================================================================================
#
# LAYER `scene` (4), the rung `gameplay/`, `world/`, `graph/` and `camera/` chose, and for
# gameplay's own stated reason: there is no `replay` layer in `engine-architecture`'s table and
# inventing one would be a change to cmake/modules.cmake that is not this phase's to make. It is
# above cy::ecs (1) because a snapshot kind is about a world, and above cy::gameplay (4, same rung)
# because a record carries a `gameplay::Command`.
#
# `ReplicationInputCursor` is in this module rather than in `src/networking/`, and that is
# deliberate: the record a peer sends is the *session's* record whether or not the networking system
# is compiled in, so `CY_NETWORKING=OFF` removes the transport that drains the cursor and not the
# cursor. A replication layer that declared its own record would be exactly the second
# representation the specification forbids, and putting the cursor behind the option would make that
# outcome the path of least resistance.
#
# ==================================================================================================
# THE DEPENDENCY LIST IS SHORT ON PURPOSE
# ==================================================================================================
#
# No renderer, no audio, no interface, no GPU. `gameplay-framework` requires headless operation of
# everything a dedicated server runs, and a replay reconstruction is the same claim:
# `replay-and-rollback` — "Reconstruction needs no renderer ... it SHALL require only recorded
# authoritative data, with no rendering or audio output needed". None of those is reachable from
# this list, so that is a property of the link graph rather than a promise.
cy_add_module(
    NAME cy_replay
    LAYER scene
    ALIAS cy::replay
    TYPE STATIC
    SOURCES
        src/crash.cpp
        src/divergence.cpp
        src/external.cpp
        src/ledger.cpp
        src/lockstep.cpp
        src/log.cpp
        src/playback.cpp
        src/readers.cpp
        src/record.cpp
        src/rollback.cpp
        src/session.cpp
        src/snapshot.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-determinism cy::core-memory cy::ecs cy::gameplay)

# See src/core/determinism/CMakeLists.txt for the argument. Deleting this line fails the configure.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cy_replay PRIVATE -ffp-contract=off)
elseif(MSVC)
    target_compile_options(cy_replay PRIVATE /fp:precise)
endif()

cy_declare_determinism_profile(cy_replay
    PROFILE SamePlatform
    REASON "a replay reproduces a session recorded by the same binary on the same machine; the cross-architecture half of the claim is NOT EVALUATED here and belongs to the CI matrix")

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
