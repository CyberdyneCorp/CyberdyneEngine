# src/servers/residency/ — layer 2: the shared residency policy. M6 tasks 4.1 to 4.3.
#
# `residency` — "Shared policy, separate storage". This module owns importance, priority scoring,
# budgets, eviction rules, hysteresis, prefetch and deadline propagation, pressure response and
# diagnostics. It owns NO page storage, NO page formats and NO page production, and the
# specification says a change that moves storage into it violates the requirement.
#
# WHY THE DEPENDENCY LIST IS THE SHORTEST IN src/servers/, AND WHY THAT IS THE POINT. A residency
# layer that could name a texture tile, a geometry page or a shadow page would be a residency layer
# with an opinion about storage. It names none of them: a page is a `Subsystem` tag and an opaque
# 56-bit number, and the only headers below are the ones a policy genuinely needs — the memory
# budget tree and the pressure monitor from cy::core-memory, and `Expected`/`Error` from
# cy::core-base. There is no cy::servers-render here, and there must not be: virtual texturing
# depends on this module, not the other way round.
#
# NOT GATED BY AN OPTION, for the same reason CyberPhysics and CyberText are not: the policy
# compiles in every build, so its suites run in every configuration rather than in the ones somebody
# remembered to switch an option on for.
#
# THE ONE INVARIANT THAT CANNOT BE RETROFITTED, per the M6 design (§3 and §5): being resident and
# being active are two independent facts about a page from the first commit. `hold()` cannot see
# activation and `set_active()` cannot change residency, and `tests/test_separation.cpp` holds bytes
# resident with nothing simulating at all — the milestone's own exit criterion.
find_package(Threads REQUIRED)

cy_add_module(
    NAME cy_servers_residency
    LAYER servers
    ALIAS cy::servers-residency
    TYPE STATIC
    SOURCES
        src/deadline.cpp
        src/importance.cpp
        src/policy.cpp
        src/request.cpp
        src/server.cpp
        src/types.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory
    PRIVATE_DEPENDENCIES cy::core-diagnostics Threads::Threads)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
