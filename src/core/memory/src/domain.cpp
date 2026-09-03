// The per-domain accounting table. Task 2.2.
//
// One array of counters, indexed by the domain enumerator. Compiled into every configuration: the
// specification requires per-domain live and peak bytes in all builds, so this file has no
// CY_DEVELOPMENT in it and must not acquire one.
//
// The counters are relaxed atomics. Nothing here orders anything else — a report is a snapshot of
// figures that are moving, and paying for acquire/release on every allocation to make the snapshot
// self-consistent would be paying for a guarantee no reader can use.

#include <cy/core/memory/domain.h>

#include <atomic>

namespace cy {
namespace {

struct DomainCounters {
    std::atomic<u64> live_bytes{0};
    std::atomic<u64> peak_bytes{0};
    std::atomic<u64> live_allocations{0};
    std::atomic<u64> total_allocations{0};
    std::atomic<u64> total_bytes{0};
    std::atomic<u64> reserved_bytes{0};
};

// Namespace-scope storage, so a leak detector sees it as a root and so the first allocation in the
// process does not have to construct it.
DomainCounters g_domains[kMemoryDomainCount];

constexpr const char* kDomainNames[kMemoryDomainCount] = {
    "engine", "ecs",    "frame",     "renderer", "gpu",     "physics",   "animation",
    "audio",  "assets", "streaming", "world",    "network", "scripting", "editor",
};

// The parent of each domain, by index. Engine is its own parent: it is the root, so walking upward
// terminates when a step does not move.
constexpr MemoryDomain kDomainParents[kMemoryDomainCount] = {
    MemoryDomain::Engine,    // Engine
    MemoryDomain::Engine,    // Ecs
    MemoryDomain::Engine,    // Frame
    MemoryDomain::Engine,    // Renderer
    MemoryDomain::Renderer,  // Gpu
    MemoryDomain::Engine,    // Physics
    MemoryDomain::Engine,    // Animation
    MemoryDomain::Engine,    // Audio
    MemoryDomain::Engine,    // Assets
    MemoryDomain::Assets,    // Streaming
    MemoryDomain::Engine,    // World
    MemoryDomain::Engine,    // Network
    MemoryDomain::Engine,    // Scripting
    MemoryDomain::Engine,    // Editor
};

[[nodiscard]] bool in_range(MemoryDomain domain) noexcept {
    return static_cast<u32>(domain) < kMemoryDomainCount;
}

/// Raise `peak` to `value` if `value` is larger. A load-then-compare-exchange loop rather than a
/// max: there is no atomic maximum, and the loop only spins when two threads set a new peak in the
/// same instant, which is rare enough that the fast path is the failed comparison.
void raise_peak(std::atomic<u64>& peak, u64 value) noexcept {
    u64 observed = peak.load(std::memory_order_relaxed);
    while (observed < value &&
           !peak.compare_exchange_weak(observed, value, std::memory_order_relaxed)) {
        // observed has been reloaded by the failed exchange; retry against the new value.
    }
}

}  // namespace

const char* domain_name(MemoryDomain domain) noexcept {
    return in_range(domain) ? kDomainNames[static_cast<u32>(domain)] : "invalid";
}

MemoryDomain domain_parent(MemoryDomain domain) noexcept {
    return in_range(domain) ? kDomainParents[static_cast<u32>(domain)] : MemoryDomain::Engine;
}

bool domain_is_within(MemoryDomain domain, MemoryDomain ancestor) noexcept {
    if (!in_range(domain) || !in_range(ancestor)) {
        return false;
    }
    MemoryDomain walk = domain;
    for (u32 step = 0; step < kMemoryDomainCount; ++step) {
        if (walk == ancestor) {
            return true;
        }
        const MemoryDomain next = domain_parent(walk);
        if (next == walk) {
            return false;  // reached the root without meeting `ancestor`
        }
        walk = next;
    }
    return false;
}

void domain_record_allocation(MemoryDomain domain, u64 bytes) noexcept {
    if (!in_range(domain)) {
        return;
    }
    DomainCounters& counters = g_domains[static_cast<u32>(domain)];
    const u64 live = counters.live_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    counters.live_allocations.fetch_add(1, std::memory_order_relaxed);
    counters.total_allocations.fetch_add(1, std::memory_order_relaxed);
    counters.total_bytes.fetch_add(bytes, std::memory_order_relaxed);
    raise_peak(counters.peak_bytes, live);
}

void domain_record_free(MemoryDomain domain, u64 bytes) noexcept {
    if (!in_range(domain)) {
        return;
    }
    DomainCounters& counters = g_domains[static_cast<u32>(domain)];
    counters.live_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    counters.live_allocations.fetch_sub(1, std::memory_order_relaxed);
}

void domain_record_reservation(MemoryDomain domain, i64 delta) noexcept {
    if (!in_range(domain)) {
        return;
    }
    DomainCounters& counters = g_domains[static_cast<u32>(domain)];
    if (delta >= 0) {
        counters.reserved_bytes.fetch_add(static_cast<u64>(delta), std::memory_order_relaxed);
    } else {
        counters.reserved_bytes.fetch_sub(static_cast<u64>(-delta), std::memory_order_relaxed);
    }
}

DomainStats domain_stats(MemoryDomain domain) noexcept {
    DomainStats out;
    if (!in_range(domain)) {
        return out;
    }
    const DomainCounters& counters = g_domains[static_cast<u32>(domain)];
    out.live_bytes = counters.live_bytes.load(std::memory_order_relaxed);
    out.peak_bytes = counters.peak_bytes.load(std::memory_order_relaxed);
    out.live_allocations = counters.live_allocations.load(std::memory_order_relaxed);
    out.total_allocations = counters.total_allocations.load(std::memory_order_relaxed);
    out.total_bytes = counters.total_bytes.load(std::memory_order_relaxed);
    out.reserved_bytes = counters.reserved_bytes.load(std::memory_order_relaxed);
    return out;
}

DomainStats domain_stats_recursive(MemoryDomain domain) noexcept {
    DomainStats out;
    if (!in_range(domain)) {
        return out;
    }
    // The table is fourteen entries and the hierarchy two deep, so a sweep costs less than any
    // structure that would avoid it, and it cannot go stale when a domain is added.
    for (u32 index = 0; index < kMemoryDomainCount; ++index) {
        const auto candidate = static_cast<MemoryDomain>(index);
        if (!domain_is_within(candidate, domain)) {
            continue;
        }
        const DomainStats stats = domain_stats(candidate);
        out.live_bytes += stats.live_bytes;
        out.peak_bytes += stats.peak_bytes;
        out.live_allocations += stats.live_allocations;
        out.total_allocations += stats.total_allocations;
        out.total_bytes += stats.total_bytes;
        out.reserved_bytes += stats.reserved_bytes;
    }
    return out;
}

void reset_domain_peaks() noexcept {
    for (auto& counters : g_domains) {
        counters.peak_bytes.store(counters.live_bytes.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
    }
}

void reset_domain_accounting() noexcept {
    for (auto& counters : g_domains) {
        counters.live_bytes.store(0, std::memory_order_relaxed);
        counters.peak_bytes.store(0, std::memory_order_relaxed);
        counters.live_allocations.store(0, std::memory_order_relaxed);
        counters.total_allocations.store(0, std::memory_order_relaxed);
        counters.total_bytes.store(0, std::memory_order_relaxed);
        counters.reserved_bytes.store(0, std::memory_order_relaxed);
    }
}

}  // namespace cy
