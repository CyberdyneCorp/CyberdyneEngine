// M9 TASK 4.2 — THE THREE MODES, THEIR PREREQUISITES, AND WHAT LOCKSTEP DOES NOT CLAIM.
//
// `networking-and-replication`'s own scenarios: "Prerequisites are checked, not assumed",
// "Mismatched participants are rejected", "The limitation is documented, not implied".
//
// THE CASE TO READ IS THE LAST ONE. It asserts what this engine refuses to claim: a session asking
// the determinism registry for cross-architecture convergence is rejected, because nothing in this
// tree has measured it. That is the milestone's brief — "lockstep is only as true as the spike says
// it is" — as a check that goes red if someone makes the claim.

#include "fixture.h"

// The umbrella header, included here so that the ONE `#if defined(CY_NETWORKING)` in the tree is
// compiled by a suite rather than only by a downstream consumer nobody has yet. Its `#error` fires
// in a build where the option is off — proven by compiling this include with the generated header's
// define removed, which is recorded in the phase's report rather than left as a claim.
#include <cy/networking/networking.h>

#include <cstring>

static_assert(cy::net::kNetworkingBuilt,
              "this suite is declared only when CY_NETWORKING is on, so the option and the module "
              "cannot disagree about whether networking is in this build");

using namespace cy::net_test;
using cy::u32;
using cy::u64;
using cy::determinism::BuildConfiguration;
using cy::determinism::DeterminismConfiguration;
using cy::determinism::DeterminismProfile;
using cy::determinism::SubsystemDeterminism;

namespace {

/// A build that meets everything the profile can ask of a build on this host: contraction off, no
/// fast-math. `deterministic_math_available` is deliberately false, because there is no such module
/// in this tree and a fixture that pretended otherwise would make the last case pass for the wrong
/// reason.
[[nodiscard]] BuildConfiguration honest_build() noexcept {
    BuildConfiguration build;
    build.contraction_off = true;
    build.fast_math = false;
    build.target_has_fma = false;
    build.deterministic_math_available = false;
    return build;
}

[[nodiscard]] SessionDeclarations everything_declared() noexcept {
    SessionDeclarations declarations;
    declarations.fixed_timestep = true;
    declarations.seeded_random_in_state = true;
    declarations.deterministic_system_order = true;
    declarations.deterministic_root_motion = true;
    declarations.snapshot_restorable = true;
    return declarations;
}

[[nodiscard]] CompatibilityScope host_scope() noexcept {
    CompatibilityScope scope;
    scope.platform = "linux";
    scope.architecture = "x86_64";
    scope.build_id = 0xC0FF'EE01ULL;
    scope.schema_set_hash = kEmptySchemaSetHash;
    scope.profile = DeterminismProfile::SamePlatform;
    return scope;
}

[[nodiscard]] cy::Status declare_deterministic(DeterminismConfiguration& configuration) noexcept {
    if (cy::Status declared = configuration.declare(
            SubsystemDeterminism{"physics", DeterminismProfile::SamePlatform, true});
        !declared) {
        return declared;
    }
    return configuration.declare(
        SubsystemDeterminism{"animation", DeterminismProfile::SamePlatform, true});
}

}  // namespace

CY_TEST_CASE("networking: a lockstep session whose subsystems can meet it is accepted") {
    DeterminismConfiguration configuration(allocator());
    CY_REQUIRE(declare_deterministic(configuration).has_value());
    // A renderer takes no part in authoritative simulation, so it constrains nothing — and it is
    // counted, so the report can say how much was waved past.
    CY_REQUIRE(
        configuration.declare(SubsystemDeterminism{"renderer", DeterminismProfile::None, false})
            .has_value());

    const auto accepted = verify_mode(NetworkMode::Lockstep, configuration, honest_build(),
                                      everything_declared(), {}, host_scope());
    CY_REQUIRE(accepted.has_value());
    CY_CHECK(accepted.value().profile_required == DeterminismProfile::SamePlatform);
    CY_CHECK_EQ(accepted.value().subsystems_examined, 3U);
    CY_CHECK_EQ(accepted.value().authoritative_subsystems, 2U);
    CY_CHECK_EQ(accepted.value().declarations_required, 5U);
    CY_CHECK_EQ(accepted.value().declarations_met, 5U);

    // And lockstep replicates commands rather than state: "bandwidth SHALL be independent of unit
    // count" is this predicate, and `scheduler.h` reads it rather than a comment.
    CY_CHECK_FALSE(replicates_state(NetworkMode::Lockstep));
    CY_CHECK(replicates_commands(NetworkMode::Lockstep));
    CY_CHECK(replicates_state(NetworkMode::SnapshotAuthoritative));
    CY_CHECK_FALSE(replicates_commands(NetworkMode::SnapshotAuthoritative));
}

CY_TEST_CASE("networking: a non-deterministic subsystem fails startup by name") {
    DeterminismConfiguration configuration(allocator());
    CY_REQUIRE(declare_deterministic(configuration).has_value());
    CY_REQUIRE(configuration
                   .declare(SubsystemDeterminism{"adaptive-budget",
                                                 DeterminismProfile::ReplayStable, true})
                   .has_value());

    const auto refused = verify_mode(NetworkMode::Lockstep, configuration, honest_build(),
                                     everything_declared(), {}, host_scope());
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().tag == ModeRefusal::DeterminismProfile);
    // Named, which is the requirement: "startup SHALL fail naming the subsystem, rather than the
    // mismatch appearing as a desync".
    CY_CHECK_EQ(std::strcmp(refused.error().subsystem, "adaptive-budget"), 0);
    CY_CHECK(refused.error().requirement[0] != '\0');

    // The same registry accepts `SnapshotAuthoritative`, which needs less. A refusal that refused
    // every mode would not be about the mode.
    CompatibilityScope scope = host_scope();
    scope.profile = DeterminismProfile::ReplayStable;
    CY_CHECK(verify_mode(NetworkMode::SnapshotAuthoritative, configuration, honest_build(),
                         SessionDeclarations{}, {}, scope)
                 .has_value());
}

CY_TEST_CASE("networking: an undeclared session prerequisite is refused, one at a time") {
    static constexpr struct {
        bool SessionDeclarations::* member;
        const char* what;
    } kEach[] = {
        {&SessionDeclarations::fixed_timestep, "fixed timestep"},
        {&SessionDeclarations::seeded_random_in_state, "seeded random"},
        {&SessionDeclarations::deterministic_system_order, "system order"},
        {&SessionDeclarations::snapshot_restorable, "snapshot restorable"},
        {&SessionDeclarations::deterministic_root_motion, "root motion"},
    };

    for (const auto& one : kEach) {
        DeterminismConfiguration configuration(allocator());
        CY_REQUIRE(declare_deterministic(configuration).has_value());
        SessionDeclarations declarations = everything_declared();
        declarations.*(one.member) = false;
        const auto refused = verify_mode(NetworkMode::Lockstep, configuration, honest_build(),
                                         declarations, {}, host_scope());
        CY_REQUIRE_FALSE(refused.has_value());
        CY_CHECK(refused.error().tag == ModeRefusal::SessionDeclaration);
    }

    // A session that declares nothing at all is refused rather than waved through, which is the
    // whole reason every field defaults false.
    DeterminismConfiguration configuration(allocator());
    CY_REQUIRE(declare_deterministic(configuration).has_value());
    CY_CHECK_FALSE(verify_mode(NetworkMode::Lockstep, configuration, honest_build(),
                               SessionDeclarations{}, {}, host_scope())
                       .has_value());
}

CY_TEST_CASE("networking: an exclusion needs a reason, and may not also be authoritative") {
    DeterminismConfiguration configuration(allocator());
    CY_REQUIRE(declare_deterministic(configuration).has_value());
    CY_REQUIRE(
        configuration.declare(SubsystemDeterminism{"vfx", DeterminismProfile::SamePlatform, true})
            .has_value());

    const ExcludedSubsystem no_reason[] = {{"vfx", ""}};
    const auto refused_reason =
        verify_mode(NetworkMode::Lockstep, configuration, honest_build(), everything_declared(),
                    cy::Span<const ExcludedSubsystem>(no_reason, 1), host_scope());
    CY_REQUIRE_FALSE(refused_reason.has_value());
    CY_CHECK(refused_reason.error().tag == ModeRefusal::ExclusionWithoutReason);

    const ExcludedSubsystem contradictory[] = {
        {"vfx", "VFX is presentation and is not re-simulated during rollback"}};
    const auto refused_contradiction =
        verify_mode(NetworkMode::Lockstep, configuration, honest_build(), everything_declared(),
                    cy::Span<const ExcludedSubsystem>(contradictory, 1), host_scope());
    CY_REQUIRE_FALSE(refused_contradiction.has_value());
    CY_CHECK(refused_contradiction.error().tag == ModeRefusal::ExcludedSubsystemIsAuthoritative);
    CY_CHECK_EQ(std::strcmp(refused_contradiction.error().subsystem, "vfx"), 0);
}

CY_TEST_CASE("networking: an incomplete compatibility scope is refused before any subsystem") {
    DeterminismConfiguration configuration(allocator());
    CY_REQUIRE(declare_deterministic(configuration).has_value());

    CompatibilityScope uncomputed = host_scope();
    uncomputed.schema_set_hash = 0;
    const auto no_schemas = verify_mode(NetworkMode::SnapshotAuthoritative, configuration,
                                        honest_build(), SessionDeclarations{}, {}, uncomputed);
    CY_REQUIRE_FALSE(no_schemas.has_value());
    CY_CHECK(no_schemas.error().tag == ModeRefusal::IncompleteScope);

    CompatibilityScope anonymous = host_scope();
    anonymous.architecture = "";
    const auto no_architecture = verify_mode(NetworkMode::Lockstep, configuration, honest_build(),
                                             everything_declared(), {}, anonymous);
    CY_REQUIRE_FALSE(no_architecture.has_value());
    CY_CHECK(no_architecture.error().tag == ModeRefusal::IncompleteScope);

    // `SnapshotAuthoritative` peers do not re-simulate, so an unnamed architecture is legitimate
    // there. A refusal that fired for every mode would not be about re-simulation.
    CompatibilityScope replayable = anonymous;
    replayable.profile = DeterminismProfile::ReplayStable;
    CY_CHECK(verify_mode(NetworkMode::SnapshotAuthoritative, configuration, honest_build(),
                         SessionDeclarations{}, {}, replayable)
                 .has_value());
}

CY_TEST_CASE("networking: a peer on another platform may watch, and may not lockstep") {
    const CompatibilityScope host = host_scope();

    CompatibilityScope other_platform = host;
    other_platform.platform = "windows";
    CY_CHECK(join_verdict(NetworkMode::SnapshotAuthoritative, host, other_platform) ==
             JoinRefusal::None);
    CY_CHECK(join_verdict(NetworkMode::Lockstep, host, other_platform) ==
             JoinRefusal::PlatformMismatch);
    CY_CHECK(join_verdict(NetworkMode::Rollback, host, other_platform) ==
             JoinRefusal::PlatformMismatch);

    CompatibilityScope other_architecture = host;
    other_architecture.architecture = "aarch64";
    CY_CHECK(join_verdict(NetworkMode::Lockstep, host, other_architecture) ==
             JoinRefusal::ArchitectureMismatch);

    CompatibilityScope other_build = host;
    other_build.build_id = 0xDEAD'BEEFULL;
    CY_CHECK(join_verdict(NetworkMode::Lockstep, host, other_build) == JoinRefusal::BuildMismatch);

    // The schema set and the profile are checked in every mode, because a peer that misreads bytes
    // is worse than a peer that cannot join.
    CompatibilityScope other_schemas = host;
    other_schemas.schema_set_hash = 0x1234ULL;
    CY_CHECK(join_verdict(NetworkMode::SnapshotAuthoritative, host, other_schemas) ==
             JoinRefusal::SchemaSetMismatch);
    CompatibilityScope other_profile = host;
    other_profile.profile = DeterminismProfile::ReplayStable;
    CY_CHECK(join_verdict(NetworkMode::SnapshotAuthoritative, host, other_profile) ==
             JoinRefusal::ProfileMismatch);

    CY_CHECK(join_verdict(NetworkMode::Lockstep, host, host) == JoinRefusal::None);
}

CY_TEST_CASE("networking: this engine does not claim cross-architecture lockstep") {
    // THE CASE THIS FILE EXISTS FOR. `networking-and-replication`: "Cross-platform lockstep SHALL
    // NOT be supported." M9's spike measured two compilers at four optimisation levels on ONE
    // architecture and design.md §1.4 says plainly that the other half is NOT EVALUATED here.
    //
    // So: lockstep asks the determinism registry for `SamePlatform`, which is what was measured...
    DeterminismConfiguration configuration(allocator());
    CY_REQUIRE(declare_deterministic(configuration).has_value());
    CY_CHECK(profile_required_by(NetworkMode::Lockstep) == DeterminismProfile::SamePlatform);
    CY_CHECK(verify_mode(NetworkMode::Lockstep, configuration, honest_build(),
                         everything_declared(), {}, host_scope())
                 .has_value());

    // ...and a session that asks for the cross-architecture guarantee instead is refused, on this
    // build, with the reason. If someone makes `profile_required_by(Lockstep)` return
    // `DeterminismProfile::Lockstep`, the case above goes red; if someone makes the engine claim
    // deterministic math it does not have, this one does.
    DeterminismConfiguration second(allocator());
    CY_REQUIRE(declare_deterministic(second).has_value());
    const auto cross = second.require(DeterminismProfile::Lockstep, honest_build());
    CY_REQUIRE_FALSE(cross.has_value());
    CY_CHECK(cross.error().tag == cy::determinism::ProfileRefusal::DeterministicMathMissing);
}
