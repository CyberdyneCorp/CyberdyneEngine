// Arming the determinism firewall M8.c built. M9 task 1.4b.

#include <cy/gameplay/firewall_arming.h>

#include <cstdio>

namespace cy::gameplay {

Status arm_write_firewall(const ecs::ComponentRegistry& registry, ecs::WriteFirewall& firewall,
                          Span<const ManualAuthority> by_hand,
                          FirewallArmingReport& report) noexcept {
    report = FirewallArmingReport{};
    report.components_registered = registry.size();

    // The hand declarations first. `declare()` refuses to *lower* a component's authority, so
    // declaring by hand before deriving means a derivation can only ever raise what the project
    // said — and a project that declared something authoritative cannot have reflection quietly
    // demote it.
    for (const ManualAuthority& manual : by_hand) {
        if (manual.reason == nullptr || manual.reason[0] == '\0') {
            return fail(ErrorCode::InvalidArgument,
                        "gameplay: a by-hand authority declaration needs a reason; an exception "
                        "list with no arguments in it is how a firewall grows a hole");
        }
        if (!registry.registered(manual.component)) {
            return fail(ErrorCode::NotFound,
                        "gameplay: a by-hand authority declaration names a component this registry "
                        "does not have; a typo here is a silent hole rather than an error");
        }
        if (Status declared = firewall.declare(manual.component, manual.authority); !declared) {
            return declared;
        }
        ++report.declared_by_hand;
    }

    ecs::AuthorityDerivationReport derivation;
    if (Status derived = firewall.declare_from_reflection(registry, derivation); !derived) {
        return derived;
    }

    report.derived_from_replication = derivation.guarded_by_replication;
    report.derived_from_persistence = derivation.guarded_by_persistence;
    report.already_declared = derivation.already_declared;
    report.underived = derivation.underived;
    report.guarded = firewall.guarded_count();
    report.armed = firewall.armed();
    return ok();
}

usize format_arming_report(char* buffer, usize capacity,
                           const FirewallArmingReport& report) noexcept {
    if (buffer == nullptr || capacity < kArmingReportBuffer) {
        return 0;
    }
    // `guarded` and `underived` on the same line and in that order. The pair is the whole of the
    // check: "armed: yes" beside "guarded: 0" is the state M8.c found, and it has to be readable at
    // a glance rather than inferred from a second line nobody prints.
    const int written = std::snprintf(
        buffer, capacity,
        "determinism firewall: armed=%s guarded=%u/%u (%u%%) underived=%u "
        "[replication=%u persistence=%u by-hand=%u already=%u]",
        report.armed ? "yes" : "no", report.guarded, report.components_registered,
        report.guarded_percent(), report.underived, report.derived_from_replication,
        report.derived_from_persistence, report.declared_by_hand, report.already_declared);
    return written > 0 ? static_cast<usize>(written) : 0;
}

}  // namespace cy::gameplay
