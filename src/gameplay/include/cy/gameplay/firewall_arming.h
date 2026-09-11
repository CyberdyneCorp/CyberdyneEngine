#pragma once
// ARMING THE DETERMINISM FIREWALL M8.c BUILT. M9 task 1.4b.
//
// ================================================================================================
// THE FINDING THIS FILE EXISTS TO CLOSE
// ================================================================================================
//
// M8.c's closing gate recorded it plainly: the runtime firewall in `<cy/ecs/firewall.h>` is **armed
// and guards nothing.** `WriteFirewall::armed()` is true out of the box, `admit()` runs on every
// write, and the whole of it is a no-op — because `guarded_count()` is zero until something calls
// `declare()` or `declare_from_reflection()`, and *the only callers in the tree are test files*. A
// game built on this engine today has a firewall that has been told nothing is authoritative and
// therefore refuses nothing.
//
// The gap is not in the firewall. It is that nobody was calling it at startup, and the reason
// nobody was is that until M9 nothing in the engine knew which components are authoritative in the
// sense that matters: **replication is the capability that knows what is on the wire.** This is the
// milestone that can answer it, so this is the milestone that arms it.
//
// ================================================================================================
// WHY THE CHECK IS A STARTUP REPORT AND NOT A UNIT TEST WITH THREE COMPONENTS IN IT
// ================================================================================================
//
// A test that registers three components, arms the firewall and asserts `guarded_count() == 2`
// proves the arithmetic and nothing else: it would pass on a tree where every real component
// derived nothing, which is precisely the state M8.c found. The number that matters is
// `guarded_count()` **beside** `AuthorityDerivationReport::underived` over a *real* registry — the
// fraction of the engine's own components the firewall actually guards — and that is a property of
// a populated world rather than of this function.
//
// So `FirewallArmingReport` is built to be *printed at startup and asserted by an artefact*, which
// is task 1.4b's own wording. `format_arming_report()` produces the line. The artefact that asserts
// it is `samples/09-multiplayer`, M9 section 6, which is a different phase's; what section 1 owes
// is the mechanism and a report whose numbers cannot be read as better than they are —
// `guarded_percent()` is reported beside `underived` for exactly that reason, and a registry where
// everything derived nothing reports 0%, loudly, rather than "armed: yes".
//
// ================================================================================================
// WHAT REFLECTION CANNOT DERIVE, AND THE HAND DECLARATION THAT COVERS IT
// ================================================================================================
//
// `declare_from_reflection()` is conservative on purpose and says so: a field carrying `Replicated`
// makes its component `Replicated`; a field that *explicitly* declares an authoritative
// `Persistence` makes it `Authoritative`; a field that declares neither derives nothing, because
// `FieldAttributes::persistence` defaults to `Authoring` and treating the default as a declaration
// would guard every reflected component on the strength of a value nobody typed.
//
// That leaves two real categories — `PhysicsOwned`, which nothing in reflection can express, and
// components with no `TypeInfo` at all (the ECS's `Parent`/`Children`, the scene's twelve
// built-ins) — and `ManualAuthority` is how a project declares them. Each carries a `reason`,
// because a by-hand authority declaration with no argument behind it is how a firewall quietly
// grows an exception list.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/component.h>
#include <cy/ecs/firewall.h>

namespace cy::gameplay {

/// A component whose authority reflection cannot derive.
struct ManualAuthority {
    ecs::ComponentTypeId component = 0;
    ecs::ComponentAuthority authority = ecs::ComponentAuthority::Presentation;
    /// Why this component is authoritative when nothing it declares says so. Required.
    const char* reason = "";
};

/// What one arming run examined, and how much of the world it ended up guarding.
struct FirewallArmingReport {
    u32 components_registered = 0;
    /// `WriteFirewall::guarded_count()` after arming. The headline.
    u32 guarded = 0;
    u32 derived_from_replication = 0;
    u32 derived_from_persistence = 0;
    u32 already_declared = 0;
    /// `AuthorityDerivationReport::underived` — components whose reflection said nothing. **Printed
    /// beside `guarded`**, because a firewall that guards a tenth of the world and one that guards
    /// all of it read identically otherwise.
    u32 underived = 0;
    u32 declared_by_hand = 0;
    bool armed = false;

    /// Guarded components as a whole percentage of those registered. Zero registered reports zero
    /// rather than a hundred: "we guarded everything there was" and "there was nothing" must not
    /// read alike.
    [[nodiscard]] u32 guarded_percent() const noexcept {
        return components_registered == 0 ? 0U : (guarded * 100U) / components_registered;
    }
};

/// Arm the firewall from what the registry's components declare, plus what the project declares by
/// hand. Called once, at session start, before the first tick.
///
/// Refuses a `ManualAuthority` with no reason and one naming an unregistered component. Both would
/// otherwise be a silent nothing, and a firewall whose exception list contains a typo is a firewall
/// with a hole in it that nobody can see.
[[nodiscard]] Status arm_write_firewall(const ecs::ComponentRegistry& registry,
                                        ecs::WriteFirewall& firewall,
                                        Span<const ManualAuthority> by_hand,
                                        FirewallArmingReport& report) noexcept;

/// The startup line, into `buffer`. Returns characters written, excluding the terminator; zero when
/// the buffer is too small, which the caller must treat as a failure rather than as an empty
/// report.
[[nodiscard]] usize format_arming_report(char* buffer, usize capacity,
                                         const FirewallArmingReport& report) noexcept;

/// The smallest buffer `format_arming_report` will always fit in.
inline constexpr usize kArmingReportBuffer = 256;

}  // namespace cy::gameplay
