#include <cy/networking/mode.h>

#include <cstring>

namespace cy::net {
namespace {

/// One required session declaration: where to read it, and what to call it in the diagnostic.
struct RequiredDeclaration {
    bool SessionDeclarations::* member;
    const char* spelled;
};

/// The declarations each mode requires, as data.
///
/// `networking-and-replication` — "Lockstep requirements and limits" — lists them, and the list is
/// reproduced here rather than restated in prose so that a mode's requirements and the diagnostic's
/// wording cannot drift apart. `Rollback` takes the subset that is about *one* peer re-simulating
/// its own inputs; root motion is lockstep's alone because a rollback client's animation is
/// presentation until the authority says otherwise.
[[nodiscard]] Span<const RequiredDeclaration> declarations_for(NetworkMode mode) noexcept {
    static constexpr RequiredDeclaration kRollback[] = {
        {&SessionDeclarations::fixed_timestep, "a fixed simulation timestep"},
        {&SessionDeclarations::seeded_random_in_state,
         "seeded random state carried in simulation state"},
        {&SessionDeclarations::deterministic_system_order, "deterministic ECS system ordering"},
        {&SessionDeclarations::snapshot_restorable, "a snapshot-restorable rollback set"},
    };
    static constexpr RequiredDeclaration kLockstep[] = {
        {&SessionDeclarations::fixed_timestep, "a fixed simulation timestep"},
        {&SessionDeclarations::seeded_random_in_state,
         "seeded random state carried in simulation state"},
        {&SessionDeclarations::deterministic_system_order, "deterministic ECS system ordering"},
        {&SessionDeclarations::snapshot_restorable, "a snapshot-restorable rollback set"},
        {&SessionDeclarations::deterministic_root_motion, "deterministic animation root motion"},
    };
    switch (mode) {
        case NetworkMode::SnapshotAuthoritative:
            return {};
        case NetworkMode::Rollback:
            return {kRollback, 4};
        case NetworkMode::Lockstep:
            return {kLockstep, 5};
    }
    return {};
}

[[nodiscard]] bool same_text(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    return std::strcmp(a, b) == 0;
}

/// Modes whose peers re-simulate. The compatibility scope is only meaningful for these — see the
/// header comment, and `join_verdict()` below, which uses the same predicate.
[[nodiscard]] constexpr bool peers_resimulate(NetworkMode mode) noexcept {
    return mode != NetworkMode::SnapshotAuthoritative;
}

}  // namespace

const char* network_mode_name(NetworkMode mode) noexcept {
    switch (mode) {
        case NetworkMode::SnapshotAuthoritative:
            return "SnapshotAuthoritative";
        case NetworkMode::Rollback:
            return "Rollback";
        case NetworkMode::Lockstep:
            return "Lockstep";
    }
    return "unknown";
}

const char* join_refusal_name(JoinRefusal refusal) noexcept {
    switch (refusal) {
        case JoinRefusal::None:
            return "None";
        case JoinRefusal::PlatformMismatch:
            return "PlatformMismatch";
        case JoinRefusal::ArchitectureMismatch:
            return "ArchitectureMismatch";
        case JoinRefusal::BuildMismatch:
            return "BuildMismatch";
        case JoinRefusal::SchemaSetMismatch:
            return "SchemaSetMismatch";
        case JoinRefusal::ProfileMismatch:
            return "ProfileMismatch";
    }
    return "unknown";
}

const char* mode_refusal_name(ModeRefusal refusal) noexcept {
    switch (refusal) {
        case ModeRefusal::None:
            return "None";
        case ModeRefusal::DeterminismProfile:
            return "DeterminismProfile";
        case ModeRefusal::SessionDeclaration:
            return "SessionDeclaration";
        case ModeRefusal::ExcludedSubsystemIsAuthoritative:
            return "ExcludedSubsystemIsAuthoritative";
        case ModeRefusal::ExclusionWithoutReason:
            return "ExclusionWithoutReason";
        case ModeRefusal::IncompleteScope:
            return "IncompleteScope";
    }
    return "unknown";
}

JoinRefusal join_verdict(NetworkMode mode, const CompatibilityScope& host,
                         const CompatibilityScope& candidate) noexcept {
    // Every mode: the wire's meaning must agree, or the peers misinterpret each other's bytes
    // rather than failing. `networking-and-replication`: "Schema identity SHALL be versioned and
    // verified at connection time."
    if (host.schema_set_hash != candidate.schema_set_hash) {
        return JoinRefusal::SchemaSetMismatch;
    }
    if (host.profile != candidate.profile) {
        return JoinRefusal::ProfileMismatch;
    }
    if (!peers_resimulate(mode)) {
        return JoinRefusal::None;
    }
    // The re-simulating modes. This is where "cross-platform lockstep SHALL NOT be supported"
    // stops being a sentence in a document.
    if (!same_text(host.platform, candidate.platform)) {
        return JoinRefusal::PlatformMismatch;
    }
    if (!same_text(host.architecture, candidate.architecture)) {
        return JoinRefusal::ArchitectureMismatch;
    }
    if (host.build_id != candidate.build_id) {
        return JoinRefusal::BuildMismatch;
    }
    return JoinRefusal::None;
}

namespace {

/// The exclusion list's own two checks. Extracted so `verify_mode()` reads as the sequence of
/// questions it asks rather than as four nested loops.
[[nodiscard]] bool exclusions_are_consistent(const determinism::DeterminismConfiguration& registry,
                                             Span<const ExcludedSubsystem> excluded,
                                             ModeRejection& rejection) noexcept {
    for (auto one : excluded) {
        if (one.reason == nullptr || one.reason[0] == '\0') {
            rejection.tag = ModeRefusal::ExclusionWithoutReason;
            rejection.subsystem = one.name;
            rejection.requirement = "an exclusion states why the subsystem is not authoritative";
            return false;
        }
        for (u32 slot = 0; slot < registry.size(); ++slot) {
            const determinism::SubsystemDeterminism& declared = registry.at(slot);
            if (declared.authoritative && same_text(declared.name, one.name)) {
                rejection.tag = ModeRefusal::ExcludedSubsystemIsAuthoritative;
                rejection.subsystem = declared.name;
                rejection.requirement =
                    "a subsystem the session excluded is declared authoritative; one of the two "
                    "declarations is wrong";
                return false;
            }
        }
    }
    return true;
}

}  // namespace

Expected<ModeReport, ModeRejection> verify_mode(
    NetworkMode mode, determinism::DeterminismConfiguration& configuration,
    const determinism::BuildConfiguration& build, const SessionDeclarations& declarations,
    Span<const ExcludedSubsystem> excluded, const CompatibilityScope& scope) noexcept {
    ModeRejection rejection;
    rejection.mode = mode;

    const Span<const RequiredDeclaration> required = declarations_for(mode);

    ModeReport report;
    report.mode = mode;
    report.profile_required = profile_required_by(mode);
    report.exclusions_examined = static_cast<u32>(excluded.size());
    report.declarations_required = static_cast<u32>(required.size());

    // The schema set is verified at connection for every mode, so a session that never computed one
    // has nothing to verify against. Zero is "not computed" — `schema.h`'s empty set hashes to a
    // non-zero constant precisely so that this test means what it says.
    if (scope.schema_set_hash == 0) {
        rejection.tag = ModeRefusal::IncompleteScope;
        rejection.requirement = "a compiled schema set identity to verify at connection time";
        return make_unexpected(rejection);
    }
    if (peers_resimulate(mode) &&
        ((scope.platform == nullptr || scope.platform[0] == '\0') ||
         (scope.architecture == nullptr || scope.architecture[0] == '\0') || scope.build_id == 0)) {
        rejection.tag = ModeRefusal::IncompleteScope;
        rejection.requirement =
            "a platform, an architecture and a build identity: peers in this mode re-simulate, and "
            "the engine does not claim that two architectures converge";
        return make_unexpected(rejection);
    }

    for (const RequiredDeclaration& one : required) {
        if (declarations.*(one.member)) {
            ++report.declarations_met;
            continue;
        }
        rejection.tag = ModeRefusal::SessionDeclaration;
        rejection.requirement = one.spelled;
        return make_unexpected(rejection);
    }

    if (!exclusions_are_consistent(configuration, excluded, rejection)) {
        rejection.mode = mode;
        return make_unexpected(rejection);
    }

    Expected<determinism::ConfigurationReport, determinism::ProfileRejection> accepted =
        configuration.require(report.profile_required, build);
    if (!accepted) {
        rejection.tag = ModeRefusal::DeterminismProfile;
        rejection.determinism = accepted.error();
        rejection.subsystem = accepted.error().subsystem;
        rejection.requirement = accepted.error().guarantee;
        return make_unexpected(rejection);
    }

    report.subsystems_examined = accepted.value().subsystems_examined;
    report.authoritative_subsystems = accepted.value().authoritative_subsystems;
    return report;
}

}  // namespace cy::net
