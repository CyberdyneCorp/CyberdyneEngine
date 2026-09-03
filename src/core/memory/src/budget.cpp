// The budget tree, the built-in platform profiles, and the startup over-subscription check.
// Task 2.2.

#include <cy/core/memory/budget.h>

#include <cstdio>

namespace cy {
namespace {

constexpr u64 kMiB = 1024ull * 1024ull;

// --- The built-in profiles
// ------------------------------------------------------------------------
//
// Three shapes rather than three numbers: a desktop with room to cache, a handheld that must not,
// and a dedicated server that has no renderer at all. The figures are starting points a project
// overrides; what they demonstrate is that the same engine runs under three apportionments with no
// code change, which is the requirement.
//
// Engine is the root and therefore the process total. Every other budget is a child of it, and Gpu
// and Streaming are children of Renderer and Assets, so their bytes are part of those totals rather
// than additional to them.

constexpr BudgetEntry kDesktopEntries[] = {
    {MemoryDomain::Engine, 4096 * kMiB, BudgetKind::Hard},
    {MemoryDomain::Ecs, 512 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Frame, 128 * kMiB, BudgetKind::Hard},
    {MemoryDomain::Renderer, 1024 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Gpu, 768 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Physics, 128 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Animation, 128 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Audio, 128 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Assets, 1024 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Streaming, 512 * kMiB, BudgetKind::Hard},
    {MemoryDomain::World, 512 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Network, 64 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Scripting, 128 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Editor, 256 * kMiB, BudgetKind::Soft},
};

constexpr BudgetEntry kHandheldEntries[] = {
    {MemoryDomain::Engine, 1024 * kMiB, BudgetKind::Hard},
    {MemoryDomain::Ecs, 128 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Frame, 32 * kMiB, BudgetKind::Hard},
    {MemoryDomain::Renderer, 256 * kMiB, BudgetKind::Hard},
    {MemoryDomain::Gpu, 192 * kMiB, BudgetKind::Hard},
    {MemoryDomain::Physics, 32 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Animation, 32 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Audio, 48 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Assets, 320 * kMiB, BudgetKind::Hard},
    {MemoryDomain::Streaming, 128 * kMiB, BudgetKind::Hard},
    {MemoryDomain::World, 96 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Network, 16 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Scripting, 32 * kMiB, BudgetKind::Soft},
};

constexpr BudgetEntry kServerEntries[] = {
    {MemoryDomain::Engine, 2048 * kMiB, BudgetKind::Hard},
    {MemoryDomain::Ecs, 640 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Frame, 64 * kMiB, BudgetKind::Hard},
    {MemoryDomain::Physics, 256 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Assets, 512 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Streaming, 256 * kMiB, BudgetKind::Hard},
    {MemoryDomain::World, 384 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Network, 128 * kMiB, BudgetKind::Soft},
    {MemoryDomain::Scripting, 64 * kMiB, BudgetKind::Soft},
};

constexpr MemoryProfile kProfiles[] = {
    {"desktop", kDesktopEntries, static_cast<u32>(sizeof(kDesktopEntries) / sizeof(BudgetEntry))},
    {"handheld", kHandheldEntries,
     static_cast<u32>(sizeof(kHandheldEntries) / sizeof(BudgetEntry))},
    {"server", kServerEntries, static_cast<u32>(sizeof(kServerEntries) / sizeof(BudgetEntry))},
};

[[nodiscard]] bool names_match(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

/// The message a failed validate() carries. One static buffer, because validate() fails once, at
/// startup, on the thread that is configuring the engine, and `cy::Error::message` does not own its
/// text (see base/error.h).
char* oversubscription_message(MemoryDomain parent, u64 parent_bytes, u64 child_sum) noexcept {
    static char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "memory budget over-subscribed: the children of '%s' sum to %llu bytes, which is "
                  "more than its own budget of %llu — lower a child or raise the parent",
                  domain_name(parent), static_cast<unsigned long long>(child_sum),
                  static_cast<unsigned long long>(parent_bytes));
    return buffer;
}

}  // namespace

const char* budget_kind_name(BudgetKind kind) noexcept {
    return (kind == BudgetKind::Hard) ? "hard" : "soft";
}

const MemoryProfile* find_memory_profile(const char* name) noexcept {
    for (const MemoryProfile& profile : kProfiles) {
        if (names_match(profile.name, name)) {
            return &profile;
        }
    }
    return nullptr;
}

const MemoryProfile* memory_profiles(u32& count) noexcept {
    count = static_cast<u32>(sizeof(kProfiles) / sizeof(MemoryProfile));
    return kProfiles;
}

void BudgetTree::set(MemoryDomain domain, u64 bytes, BudgetKind kind) noexcept {
    const u32 index = static_cast<u32>(domain);
    if (index >= kMemoryDomainCount) {
        return;
    }
    entries_[index].bytes = bytes;
    entries_[index].kind = kind;
}

void BudgetTree::clear() noexcept {
    for (Entry& entry : entries_) {
        entry = Entry{};
    }
}

Status BudgetTree::apply(const MemoryProfile& profile) noexcept {
    clear();
    for (u32 index = 0; index < profile.entry_count; ++index) {
        const BudgetEntry& entry = profile.entries[index];
        set(entry.domain, entry.bytes, entry.kind);
    }
    return validate();
}

Status BudgetTree::validate() const noexcept {
    // For each domain that has a budget, the budgets of its direct children must fit inside it.
    // Direct children only: a grandchild is already inside its own parent's total, and counting it
    // twice would reject a correct configuration.
    for (u32 parent = 0; parent < kMemoryDomainCount; ++parent) {
        const auto parent_domain = static_cast<MemoryDomain>(parent);
        if (entries_[parent].bytes == 0) {
            continue;
        }
        u64 child_sum = 0;
        for (u32 child = 0; child < kMemoryDomainCount; ++child) {
            const auto child_domain = static_cast<MemoryDomain>(child);
            if (child == parent || domain_parent(child_domain) != parent_domain) {
                continue;
            }
            child_sum += entries_[child].bytes;
        }
        if (child_sum > entries_[parent].bytes) {
            return fail(ErrorCode::InvalidArgument,
                        oversubscription_message(parent_domain, entries_[parent].bytes, child_sum));
        }
    }
    return ok();
}

u64 BudgetTree::budget(MemoryDomain domain) const noexcept {
    const u32 index = static_cast<u32>(domain);
    return (index < kMemoryDomainCount) ? entries_[index].bytes : 0;
}

BudgetKind BudgetTree::kind(MemoryDomain domain) const noexcept {
    const u32 index = static_cast<u32>(domain);
    return (index < kMemoryDomainCount) ? entries_[index].kind : BudgetKind::Soft;
}

bool BudgetTree::has_budget(MemoryDomain domain) const noexcept {
    return budget(domain) != 0;
}

f64 BudgetTree::utilisation(MemoryDomain domain) const noexcept {
    const u64 allowance = budget(domain);
    if (allowance == 0) {
        return 0.0;
    }
    const DomainStats stats = domain_stats_recursive(domain);
    return static_cast<f64>(stats.live_bytes) / static_cast<f64>(allowance);
}

bool BudgetTree::over_budget(MemoryDomain domain) const noexcept {
    const u64 allowance = budget(domain);
    if (allowance == 0) {
        return false;
    }
    return domain_stats_recursive(domain).live_bytes > allowance;
}

f64 BudgetTree::peak_utilisation(MemoryDomain& worst) const noexcept {
    f64 highest = 0.0;
    worst = MemoryDomain::Engine;
    for (u32 index = 0; index < kMemoryDomainCount; ++index) {
        const auto domain = static_cast<MemoryDomain>(index);
        const f64 value = utilisation(domain);
        if (value > highest) {
            highest = value;
            worst = domain;
        }
    }
    return highest;
}

bool BudgetTree::admits(MemoryDomain domain, u64 bytes) const noexcept {
    // Every enclosing hard budget has to admit the growth, not just this domain's: a texture cache
    // inside a hard renderer budget is limited by the renderer's figure even when its own is soft.
    MemoryDomain walk = domain;
    for (u32 step = 0; step < kMemoryDomainCount; ++step) {
        const u64 allowance = budget(walk);
        if (allowance != 0 && kind(walk) == BudgetKind::Hard &&
            domain_stats_recursive(walk).live_bytes + bytes > allowance) {
            return false;
        }
        const MemoryDomain next = domain_parent(walk);
        if (next == walk) {
            break;
        }
        walk = next;
    }
    return true;
}

void BudgetTree::record_eviction(MemoryDomain domain, u64 bytes) noexcept {
    const u32 index = static_cast<u32>(domain);
    if (index >= kMemoryDomainCount) {
        return;
    }
    entries_[index].evicted_bytes += bytes;
    ++entries_[index].evictions;
}

u32 BudgetTree::report(BudgetRow* out, u32 capacity) const noexcept {
    u32 written = 0;
    for (u32 index = 0; index < kMemoryDomainCount && written < capacity; ++index) {
        if (entries_[index].bytes == 0) {
            continue;
        }
        const auto domain = static_cast<MemoryDomain>(index);
        const DomainStats stats = domain_stats_recursive(domain);
        BudgetRow& row = out[written++];
        row.domain = domain;
        row.budget = entries_[index].bytes;
        row.kind = entries_[index].kind;
        row.live_bytes = stats.live_bytes;
        row.peak_bytes = stats.peak_bytes;
        row.utilisation = static_cast<f64>(stats.live_bytes) / static_cast<f64>(row.budget);
        row.over_budget = stats.live_bytes > row.budget;
        row.evicted_bytes = entries_[index].evicted_bytes;
        row.evictions = entries_[index].evictions;
    }
    return written;
}

BudgetTree& default_budget_tree() noexcept {
    static BudgetTree tree;
    return tree;
}

}  // namespace cy
