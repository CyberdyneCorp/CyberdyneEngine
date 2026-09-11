// Determinism profiles, and the refusal that happens at configuration. M9 tasks 2.1 and 2.2.

#include <cy/core/determinism/profile.h>

#include <cmath>
#include <cstring>
#include <utility>

namespace cy::determinism {
namespace {

/// The obligations, in the order the diagnostic should report them: the cheapest to fix first, so
/// a project stepping up a profile is told about recording before it is told about arithmetic.
struct Obligation {
    bool Guarantees::* field;
    const char* spelling;
};

constexpr Obligation kObligations[] = {
    {&Guarantees::records_commands, "command recording"},
    {&Guarantees::records_external_results, "external-result recording"},
    {&Guarantees::same_binary_reproducible, "same-binary reproducibility"},
    {&Guarantees::cross_platform_reproducible, "cross-platform reproducibility"},
    {&Guarantees::commands_alone, "reconstruction from commands alone"},
};

[[nodiscard]] bool name_precedes(const char* left, const char* right) noexcept {
    return std::strcmp(left, right) < 0;
}

}  // namespace

const char* determinism_profile_name(DeterminismProfile profile) noexcept {
    switch (profile) {
        case DeterminismProfile::None:
            return "None";
        case DeterminismProfile::ReplayStable:
            return "ReplayStable";
        case DeterminismProfile::SamePlatform:
            return "SamePlatform";
        case DeterminismProfile::CrossPlatform:
            return "CrossPlatform";
        case DeterminismProfile::Lockstep:
            return "Lockstep";
    }
    return "None";
}

const char* profile_refusal_name(ProfileRefusal refusal) noexcept {
    switch (refusal) {
        case ProfileRefusal::None:
            return "None";
        case ProfileRefusal::SubsystemGuarantee:
            return "SubsystemGuarantee";
        case ProfileRefusal::FloatContraction:
            return "FloatContraction";
        case ProfileRefusal::FastMath:
            return "FastMath";
        case ProfileRefusal::DeterministicMathMissing:
            return "DeterministicMathMissing";
        case ProfileRefusal::NoSubsystemsDeclared:
            return "NoSubsystemsDeclared";
        case ProfileRefusal::Count:
            break;
    }
    return "None";
}

const char* unmet_guarantee(DeterminismProfile provided, DeterminismProfile required) noexcept {
    const Guarantees have = guarantees_of(provided);
    const Guarantees want = guarantees_of(required);
    for (const Obligation& obligation : kObligations) {
        if (want.*(obligation.field) && !(have.*(obligation.field))) {
            return obligation.spelling;
        }
    }
    return nullptr;
}

Status DeterminismConfiguration::declare(const SubsystemDeterminism& subsystem) noexcept {
    if (finalized_) {
        return fail(ErrorCode::PermissionDenied,
                    "determinism: a subsystem declared after the configuration was finalised");
    }
    if (subsystem.name == nullptr || subsystem.name[0] == '\0') {
        return fail(ErrorCode::InvalidArgument,
                    "determinism: a subsystem needs a name to be named "
                    "by a refusal");
    }
    for (const SubsystemDeterminism& existing : subsystems_) {
        if (std::strcmp(existing.name, subsystem.name) == 0) {
            return fail(ErrorCode::AlreadyExists, "determinism: subsystem declared twice");
        }
    }
    return subsystems_.push_back(subsystem);
}

void DeterminismConfiguration::finalize() noexcept {
    if (finalized_) {
        return;
    }
    // Insertion sort by name. Stable, allocates nothing, and the list is the number of subsystems
    // in an engine rather than a population. What matters is that it runs before anything reads the
    // list, so the subsystem a refusal names does not depend on declaration order.
    const usize count = subsystems_.size();
    for (usize i = 1; i < count; ++i) {
        for (usize j = i; j > 0 && name_precedes(subsystems_[j].name, subsystems_[j - 1].name);
             --j) {
            std::swap(subsystems_[j - 1], subsystems_[j]);
        }
    }
    finalized_ = true;
}

DeterminismProfile DeterminismConfiguration::strongest_supported() const noexcept {
    DeterminismProfile best = DeterminismProfile::Lockstep;
    bool any = false;
    for (const SubsystemDeterminism& subsystem : subsystems_) {
        if (!subsystem.authoritative) {
            continue;
        }
        any = true;
        if (!satisfies(subsystem.guarantees, best)) {
            best = subsystem.guarantees;
        }
    }
    return any ? best : DeterminismProfile::None;
}

Expected<ConfigurationReport, ProfileRejection> DeterminismConfiguration::require(
    DeterminismProfile profile, const BuildConfiguration& build) noexcept {
    finalize();

    ConfigurationReport report;
    report.profile = profile;
    report.subsystems_examined = static_cast<u32>(subsystems_.size());

    if (profile == DeterminismProfile::None) {
        // Nothing is promised, so nothing can fail to be met. Counted anyway: the report says how
        // many subsystems a `None` session declined to constrain.
        for (const SubsystemDeterminism& subsystem : subsystems_) {
            if (subsystem.authoritative) {
                ++report.authoritative_subsystems;
            }
        }
        return report;
    }

    if (subsystems_.empty()) {
        // A session that required `Lockstep` of an empty registry would otherwise be accepted, and
        // a check that passes when it has been told nothing is the shape of every criterion this
        // project's gates have had to remove.
        return make_unexpected(ProfileRejection{ProfileRefusal::NoSubsystemsDeclared, "",
                                                "at least one declared subsystem", profile,
                                                DeterminismProfile::None});
    }

    // --- The build half, first. -----------------------------------------------------------------
    //
    // Before any subsystem is blamed, because a build compiled with fast-math would fail for every
    // subsystem in the list and naming the alphabetically first one would send the reader to the
    // wrong place entirely.
    if (build.fast_math) {
        return make_unexpected(ProfileRejection{ProfileRefusal::FastMath, "",
                                                "fast-math transformations disallowed on "
                                                "authoritative paths",
                                                profile, DeterminismProfile::None});
    }
    const Guarantees wanted = guarantees_of(profile);
    if (wanted.same_binary_reproducible && build.target_has_fma && !build.contraction_off) {
        // The spike's finding, as a refusal. On a target with an FMA the compiler is free to
        // contract `a * b + c`, the two compilers make different choices, and 231 of 267 values
        // moved in the spike's `state_hash` workload when it was allowed to.
        return make_unexpected(ProfileRejection{ProfileRefusal::FloatContraction, "",
                                                "-ffp-contract=off on a target with a fused "
                                                "multiply-add",
                                                profile, DeterminismProfile::None});
    }
    if (wanted.cross_platform_reproducible && !build.deterministic_math_available) {
        // `simulation-and-determinism`: "The engine SHALL NOT claim that arbitrary floating-point
        // code produces identical results across architectures, compilers, or vector widths."
        // There is no deterministic math module in this tree, so this refusal fires for every
        // `CrossPlatform` session today, and that is the honest answer rather than a silent pass.
        return make_unexpected(ProfileRejection{ProfileRefusal::DeterministicMathMissing, "",
                                                "deterministic math types for authoritative "
                                                "computation",
                                                profile, DeterminismProfile::None});
    }

    // --- Then the subsystems, in name order. ----------------------------------------------------
    for (const SubsystemDeterminism& subsystem : subsystems_) {
        if (!subsystem.authoritative) {
            continue;
        }
        ++report.authoritative_subsystems;
        if (const char* unmet = unmet_guarantee(subsystem.guarantees, profile); unmet != nullptr) {
            return make_unexpected(ProfileRejection{ProfileRefusal::SubsystemGuarantee,
                                                    subsystem.name, unmet, profile,
                                                    subsystem.guarantees});
        }
        if (profile != DeterminismProfile::Lockstep &&
            unmet_guarantee(subsystem.guarantees, static_cast<DeterminismProfile>(
                                                      static_cast<u8>(profile) + 1)) != nullptr) {
            ++report.at_the_limit;
        }
    }
    return report;
}

f32 NonFiniteGuard::check(f32 value, const char* field) noexcept {
    ++checked_;
    if (!std::isfinite(value)) {
        ++non_finite_;
        if (first_field_[0] == '\0') {
            first_field_ = field;
        }
    }
    return value;
}

f64 NonFiniteGuard::check(f64 value, const char* field) noexcept {
    ++checked_;
    if (!std::isfinite(value)) {
        ++non_finite_;
        if (first_field_[0] == '\0') {
            first_field_ = field;
        }
    }
    return value;
}

void NonFiniteGuard::clear() noexcept {
    checked_ = 0;
    non_finite_ = 0;
    first_field_ = "";
}

}  // namespace cy::determinism
