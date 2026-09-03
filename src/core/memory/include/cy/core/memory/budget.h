#pragma once
// The memory budget tree. Task 2.2.
//
// `core-memory-and-containers` — "Memory budget tree": a total budget for the process, apportioned
// to domains and sub-domains, configurable per platform profile. Each budget is declared hard or
// soft. A soft budget being exceeded raises pressure; a hard budget is not exceeded — the owning
// system evicts, refuses or degrades instead. Budgets are reportable as target against actual, and
// child budgets summing to more than their parent is a configuration error detected AT STARTUP
// rather than at the moment of failure.
//
// The tree's shape is `domain.h`'s: fourteen domains, two of them children. It carries a number and
// a kind per domain and nothing else, because everything it compares against is already in the
// domain accounting table.
//
// WHY startup VALIDATION IS SEPARATE FROM SETTING. `set()` cannot reject an over-subscription on
// its own: a configuration that sets the children before the parent would be rejected halfway
// through a state that is legal once it is finished. `validate()` is therefore a whole-tree pass
// the caller runs when the configuration is complete, and `apply()` runs it for a profile.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/domain.h>

namespace cy {

/// What exceeding a budget means.
///
/// `Soft` is a target: crossing it raises pressure so that caches trim and prefetching backs off.
/// `Hard` is a limit: the owning system asks `admits()` before it grows, and answers the refusal
/// by evicting, refusing or degrading. Nothing here enforces a hard budget by failing an
/// allocation, because by the time an allocation fails the system has already failed.
enum class BudgetKind : u8 {
    Soft = 0,
    Hard = 1,
};

[[nodiscard]] const char* budget_kind_name(BudgetKind kind) noexcept;

/// One domain's budget, as a platform profile states it.
struct BudgetEntry {
    MemoryDomain domain = MemoryDomain::Engine;
    u64 bytes = 0;
    BudgetKind kind = BudgetKind::Soft;
};

/// A platform's whole apportionment. `entries` points at storage that outlives the profile — the
/// built-in profiles are static tables, and a project's own comes from its configuration, which is
/// alive for as long as the budgets derived from it.
struct MemoryProfile {
    const char* name = "";
    const BudgetEntry* entries = nullptr;
    u32 entry_count = 0;
};

/// The profiles the engine ships with, by name: "desktop", "handheld", "server". Null when the name
/// is not one of them — which a project's configuration turns into a startup error naming what it
/// asked for, rather than silently running unbudgeted.
[[nodiscard]] const MemoryProfile* find_memory_profile(const char* name) noexcept;
/// Every built-in profile, so a report or a test can enumerate them rather than restate the list.
[[nodiscard]] const MemoryProfile* memory_profiles(u32& count) noexcept;

/// One row of a budget report: the target, the actual, and whether the two are in the wrong order.
struct BudgetRow {
    MemoryDomain domain = MemoryDomain::Engine;
    u64 budget = 0;
    BudgetKind kind = BudgetKind::Soft;
    u64 live_bytes = 0;     // including children
    u64 peak_bytes = 0;     // including children
    f64 utilisation = 0.0;  // live / budget; zero when the domain has no budget
    bool over_budget = false;
    u64 evicted_bytes = 0;
    u64 evictions = 0;
};

/// The apportionment, and the comparison of it against what the domains actually hold.
///
/// Not internally synchronised for writes: a budget is configured at startup and by the residency
/// policy, both of which are single-threaded moments. Reads are lock-free — `utilisation()` is
/// called every frame by the pressure monitor — and read a `u64` that a writer stores whole.
class BudgetTree {
public:
    BudgetTree() noexcept = default;

    BudgetTree(const BudgetTree&) = delete;
    BudgetTree& operator=(const BudgetTree&) = delete;

    /// Set one domain's budget. Zero bytes means "unbudgeted", which is not the same as a budget of
    /// zero and is what a domain a platform does not use should have.
    void set(MemoryDomain domain, u64 bytes, BudgetKind kind) noexcept;
    void clear() noexcept;

    /// Set every budget from a profile and validate the result. The whole of "platform profiles
    /// differ": a platform with less memory supplies a different profile and no code changes.
    Status apply(const MemoryProfile& profile) noexcept;

    /// The startup check. Fails when any domain's children sum to more than it does, naming the
    /// parent, the parent's budget and the sum — because "which child is too big" is the next
    /// question and a message that omits the numbers cannot answer it.
    [[nodiscard]] Status validate() const noexcept;

    [[nodiscard]] u64 budget(MemoryDomain domain) const noexcept;
    [[nodiscard]] BudgetKind kind(MemoryDomain domain) const noexcept;
    [[nodiscard]] bool has_budget(MemoryDomain domain) const noexcept;

    /// Live bytes over budget, in [0, ∞). Zero when the domain is unbudgeted, so an unbudgeted
    /// domain never contributes to pressure — it has not been told what it may use.
    [[nodiscard]] f64 utilisation(MemoryDomain domain) const noexcept;
    [[nodiscard]] bool over_budget(MemoryDomain domain) const noexcept;

    /// The largest utilisation over every budgeted domain, and which domain it was. This is the
    /// figure the pressure monitor derives its level from: one domain at 99% is a problem even when
    /// the process total is comfortable.
    [[nodiscard]] f64 peak_utilisation(MemoryDomain& worst) const noexcept;

    /// Whether `domain` may grow by `bytes` without crossing a hard budget. A soft budget always
    /// admits — crossing it is what raises pressure, not what stops the growth.
    [[nodiscard]] bool admits(MemoryDomain domain, u64 bytes) const noexcept;

    /// Record that a cache evicted rather than allocated. The specification requires the eviction
    /// be reported; this is where a report reads it from.
    void record_eviction(MemoryDomain domain, u64 bytes) noexcept;

    /// Fill `out` with one row per budgeted domain, in domain order. Returns the number written,
    /// which is never more than `capacity`.
    u32 report(BudgetRow* out, u32 capacity) const noexcept;

private:
    struct Entry {
        u64 bytes = 0;
        u64 evicted_bytes = 0;
        u64 evictions = 0;
        BudgetKind kind = BudgetKind::Soft;
    };

    Entry entries_[kMemoryDomainCount] = {};
};

/// The process's budget tree. A single instance, because a budget that is not the whole process's
/// apportionment is not a budget.
[[nodiscard]] BudgetTree& default_budget_tree() noexcept;

}  // namespace cy
