# src/networking/ — CyberNet: the network modes, the authority model, the transports, the compiled
# replication schemas, interest and the priority scheduler, prediction and reconciliation, and the
# dedicated server. M9 section 4.
#
# ==================================================================================================
# THIS DIRECTORY IS WHAT `CY_NETWORKING` GATES, AND UNTIL M9 IT GATED NOTHING
# ==================================================================================================
#
# `CY_NETWORKING` has been declared in cmake/features.cmake since M0. Until this milestone there was
# no `if(CY_NETWORKING)` and no `#if defined(CY_NETWORKING)` anywhere in the tree, so the option and
# its absence produced byte-identical output — which is precisely the shape M8.b's closing gate found
# in `CY_UI` and M8.c had to repair. `src/CMakeLists.txt` now reads this directory only when the
# option is on, and the option defaults ON because `delivery-roadmap` fails a capability at Working
# whose CY_* option defaults off.
#
# **Both directions are built and tested**, which is the half that was never done for `CY_UI`:
# `-D CY_NETWORKING=OFF` removes this directory and its three suites and nothing else changes,
# because nothing below layer 4 names this module and `src/replay/`'s `ReplicationInputCursor` — the
# record replication reads — is deliberately NOT behind the option. The option removes the
# transport, not the record.
#
# ==================================================================================================
# LAYER, AND THE DEPENDENCY LIST THE DEDICATED SERVER MAKES A LINK-GRAPH FACT
# ==================================================================================================
#
# LAYER `scene` (4), the rung `gameplay/`, `world/`, `graph/`, `camera/` and `replay/` chose, and for
# the reason src/gameplay/CMakeLists.txt gives: there is no `networking` rung in
# `engine-architecture`'s table and inventing one would be a change to cmake/modules.cmake that is
# not this phase's to make.
#
# There is **no renderer, no audio, no interface and no GPU** in the list below, and that is the
# dedicated server requirement rather than an aesthetic: "A dedicated server build SHALL exclude:
# the renderer, VFX, UI, client audio, and the editor." None of those is reachable from
# `cy::core-base cy::core-determinism cy::core-memory cy::core-reflect cy::ecs cy::gameplay
# cy::replay`, so the exclusion is a property of the link graph that `just quality-layers` checks,
# not a promise in a document. `tests/test_server.cpp` asserts the same thing from the source side
# with `__has_include`, the way src/gameplay/tests/test_bypass.cpp does.
cy_add_module(
    NAME cy_networking
    LAYER scene
    ALIAS cy::networking
    TYPE STATIC
    SOURCES
        src/authority.cpp
        src/interest.cpp
        src/local_transport.cpp
        src/mode.cpp
        src/prediction.cpp
        src/profiler.cpp
        src/reliability.cpp
        src/replication.cpp
        src/rpc.cpp
        src/scheduler.cpp
        src/schema.cpp
        src/server.cpp
        src/udp_transport.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES
        cy::core-base cy::core-determinism cy::core-memory cy::core-reflect cy::ecs cy::gameplay
        cy::replay)

# The determinism profile this module participates in, and the flag that makes the declaration
# mean something. See src/core/determinism/CMakeLists.txt for the argument: the function does not
# add the flag, it fails the configure when it is missing.
#
# WHY `SamePlatform` AND NOT `Lockstep`. Lockstep's guarantee is that peers reproduce identical state
# from commands alone, and M9's spike measured that on ONE architecture with ONE libm. Nothing in
# this tree has compared a state hash between two architectures, so this module declares what has
# been measured and `mode.h`'s `CompatibilityScope` refuses a peer that differs in platform,
# architecture or build rather than claiming convergence nobody has observed.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cy_networking PRIVATE -ffp-contract=off)
elseif(MSVC)
    target_compile_options(cy_networking PRIVATE /fp:precise)
endif()

cy_declare_determinism_profile(cy_networking
    PROFILE SamePlatform
    REASON "replication, prediction and lockstep hash comparison are reproducible for one build on one architecture; the cross-architecture half is NOT EVALUATED on this host and belongs to the CI matrix")

if(CY_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
