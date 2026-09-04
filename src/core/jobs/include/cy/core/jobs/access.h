#pragma once
// Access declarations — the invariant this milestone exists to establish. Task 3.2.2, design.md §3.
//
// `core-jobs-and-concurrency`: "Systems SHALL declare their data access as part of their signature:
// for each component type, Read, Write, or Exclude; plus declared access to resources (singleton
// state) and event channels. The scheduler SHALL build a dependency graph per stage from these
// declarations and run systems in parallel when their access sets do not conflict."
//
// WHY THIS LANDS AT M1, BEFORE THERE IS A SYSTEM TO SCHEDULE. The property is *by construction*: it
// constrains how a system is written. A system written first and declared afterwards is written in
// a shape that may not admit a declaration — it reaches for a global, it mutates a component it
// only meant to read, it spawns an entity in the middle of a loop. Deferring the model to M2 is the
// tempting order and the wrong one, so the declarations, the conflict checker and the deferred
// command buffer land here and are exercised by synthetic systems until M2 brings real ones.
//
// WHAT CONFLICTS
//
//   Read  vs Read     never conflicts. Any number of systems may read one component concurrently.
//   Write vs anything conflicts, on the same component, resource or event channel.
//   Exclude vs anything does NOT conflict, and that is a decision rather than an oversight: an
//                     Exclude reads no component data. It is a query filter over *presence*, and
//                     presence changes only through a structural change, which the specification
//                     already forbids during parallel execution. Two systems, one excluding
//                     `Frozen` and one writing it, therefore run concurrently and correctly.
//
// A conflict is not a rejection. Two systems that both write `Transform` are *ordered* — by an
// explicit constraint where one exists and by a stable registration order otherwise. What IS
// rejected, at registration, is a declaration that cannot be scheduled at all: a system that
// declares two different accesses to the same thing, a duplicate name, an ordering constraint on a
// system that does not exist, and an ordering constraint that closes a cycle. Nothing about a
// system's access is discovered while it runs.

#include <cy/core/base/assert.h>
#include <cy/core/jobs/types.h>

namespace cy::jobs {

/// An opaque identifier for a component type. M2's ECS supplies real ones from the reflection
/// manifest; nothing here derives meaning from the number, which is why a synthetic system can use
/// any distinct value.
using ComponentTypeId = u32;
/// Singleton state a system reads or writes — a resource, in the specification's vocabulary.
using ResourceId = u32;
/// An event channel a system reads from or writes to.
using EventChannelId = u32;

/// What a declaration is about. Three separate identifier spaces, so component 3 and resource 3 are
/// different things and do not conflict with each other.
enum class AccessDomain : u8 {
    Component = 0,
    Resource = 1,
    EventChannel = 2,
};

const char* access_domain_name(AccessDomain domain) noexcept;

enum class Access : u8 {
    Read = 0,
    Write = 1,
    /// A query filter: entities holding this component are excluded. Reads no component data, and
    /// therefore conflicts with nothing. See the header comment.
    Exclude = 2,
};

const char* access_name(Access access) noexcept;

struct AccessEntry {
    AccessDomain domain = AccessDomain::Component;
    Access access = Access::Read;
    u32 id = 0;
};

/// Why two systems cannot run at the same time, in a form a diagnostic can print.
struct AccessConflict {
    AccessDomain domain = AccessDomain::Component;
    u32 id = 0;
    Access first = Access::Read;
    Access second = Access::Read;
};

/// One system's declared access. A fixed-capacity, sorted set: no allocation, and a conflict test
/// that is a linear merge of two small arrays rather than a hash lookup per entry.
class AccessSet {
public:
    /// Entries one system may declare. A system touching more than this many distinct things is
    /// almost certainly two systems.
    static constexpr u32 kMaxEntries = 32;

    constexpr AccessSet() noexcept = default;

    /// Declare one access. Fails with InvalidArgument when the same thing is already declared with
    /// a different access — that is a contradictory declaration and there is no order in which it
    /// is satisfiable — and with AlreadyExists when it is declared identically twice, which is a
    /// copy-paste rather than an intention.
    Status declare(AccessDomain domain, u32 id, Access access) noexcept;

    Status read(ComponentTypeId type) noexcept {
        return declare(AccessDomain::Component, type, Access::Read);
    }
    Status write(ComponentTypeId type) noexcept {
        return declare(AccessDomain::Component, type, Access::Write);
    }
    Status exclude(ComponentTypeId type) noexcept {
        return declare(AccessDomain::Component, type, Access::Exclude);
    }
    Status resource_read(ResourceId resource) noexcept {
        return declare(AccessDomain::Resource, resource, Access::Read);
    }
    Status resource_write(ResourceId resource) noexcept {
        return declare(AccessDomain::Resource, resource, Access::Write);
    }
    Status event_read(EventChannelId channel) noexcept {
        return declare(AccessDomain::EventChannel, channel, Access::Read);
    }
    Status event_write(EventChannelId channel) noexcept {
        return declare(AccessDomain::EventChannel, channel, Access::Write);
    }

    [[nodiscard]] u32 size() const noexcept { return count_; }
    [[nodiscard]] const AccessEntry& entry(u32 index) const noexcept { return entries_[index]; }

    /// The declared access to one thing, or false when it was not declared at all.
    [[nodiscard]] bool find(AccessDomain domain, u32 id, Access& out) const noexcept;

    /// True when the two sets cannot run at the same time, with the reason in `out`.
    [[nodiscard]] bool conflicts_with(const AccessSet& other, AccessConflict& out) const noexcept;

    void clear() noexcept { count_ = 0; }

private:
    /// Sorted by (domain, id), which is what makes `conflicts_with` a single merge pass.
    AccessEntry entries_[kMaxEntries] = {};
    u32 count_ = 0;
};

// --- The run-time half: undeclared access is caught
// -----------------------------------------------
//
// `core-jobs-and-concurrency`: "WHEN a development build detects a system touching a component it
// did not declare THEN an assertion SHALL fire identifying the system and the component type."
//
// The assertion is here, and so is a counter, for the reason stated everywhere else in this module:
// CY_ASSERT is compiled out of Profile and Shipping, so a suite written against the assertion alone
// would be vacuous in exactly the two configurations where an undeclared access is least likely to
// be noticed. The counter is compiled into all four.

/// Binds a system's name to its declaration for the duration of its body.
class SystemAccessGuard {
public:
    /// The guard of a body that has no declaration to check against. Every check fails, which is
    /// the honest answer: a body running outside a schedule declared nothing.
    constexpr SystemAccessGuard() noexcept = default;

    constexpr SystemAccessGuard(const char* system, const AccessSet& access) noexcept
        : system_(system), access_(&access) {}

    /// True when `requested` access to this thing was declared. Otherwise records the violation —
    /// the system, the domain, the identifier and the access it attempted — and returns false.
    ///
    /// A Read where Write was declared is permitted: a writer may read what it writes. A Write
    /// where only Read was declared is not.
    [[nodiscard]] bool check(AccessDomain domain, u32 id, Access requested) const noexcept;

    [[nodiscard]] const char* system() const noexcept { return system_; }

private:
    const char* system_ = "";
    const AccessSet* access_ = nullptr;
};

u64 undeclared_access_violations() noexcept;
/// The system named by the most recent violation, or "" when there has been none.
const char* last_undeclared_access_system() noexcept;
/// The identifier the most recent violation touched.
u32 last_undeclared_access_id() noexcept;
void reset_undeclared_access_violations() noexcept;

}  // namespace cy::jobs

/// Assert that a system declared the access it is about to make.
///
///     CY_ASSERT_DECLARED_ACCESS(guard, cy::jobs::AccessDomain::Component, kVelocity,
///                               cy::jobs::Access::Write);
///
/// CY_VERIFY, not CY_ASSERT: the expression must be evaluated in every configuration, because the
/// call is what records the violation.
#define CY_ASSERT_DECLARED_ACCESS(guard, domain, id, access) \
    CY_VERIFY((guard).check((domain), (id), (access)))
