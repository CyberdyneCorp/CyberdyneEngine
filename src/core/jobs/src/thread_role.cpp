// Thread roles, and the counted enforcement behind CY_ASSERT_THREAD_ROLE. Task 3.2.1.
//
// The role set is a thread_local bitmask rather than a single value, because the specification
// allows one OS thread to hold both Main and Simulation where the platform's message pump requires
// it. Everything else is one bit.
//
// The violation counter is a relaxed atomic and is compiled into every configuration. That is the
// whole point of it: `CY_ASSERT` is compiled out of Profile and Shipping, so a suite written
// against the assertion alone would report nothing in the two configurations where a role violation
// is least likely to be noticed.

#include <cy/core/jobs/thread_role.h>

#include <atomic>

namespace cy::jobs {
namespace {

thread_local u32 t_role_mask = 0;
thread_local ThreadRole t_primary_role = ThreadRole::Unknown;
thread_local WorkerIndex t_worker = kNotAWorker;

std::atomic<u64> g_violations{0};
std::atomic<const char*> g_last_violation{""};
std::atomic<ThreadRole> g_last_required{ThreadRole::Unknown};

constexpr u32 role_bit(ThreadRole role) noexcept {
    return 1u << static_cast<u32>(role);
}

}  // namespace

const char* thread_role_name(ThreadRole role) noexcept {
    switch (role) {
        case ThreadRole::Unknown:
            return "Unknown";
        case ThreadRole::Main:
            return "Main";
        case ThreadRole::Simulation:
            return "Simulation";
        case ThreadRole::Worker:
            return "Worker";
        case ThreadRole::Render:
            return "Render";
        case ThreadRole::Audio:
            return "Audio";
        case ThreadRole::AssetIo:
            return "AssetIo";
    }
    return "Unknown";
}

void set_thread_role(ThreadRole role, WorkerIndex worker) noexcept {
    t_role_mask |= role_bit(role);
    if (t_primary_role == ThreadRole::Unknown) {
        t_primary_role = role;
    }
    if (worker != kNotAWorker) {
        t_worker = worker;
    }
}

void clear_thread_roles() noexcept {
    t_role_mask = 0;
    t_primary_role = ThreadRole::Unknown;
    t_worker = kNotAWorker;
}

ThreadRole current_thread_role() noexcept {
    return t_primary_role;
}

bool thread_holds_role(ThreadRole role) noexcept {
    return (t_role_mask & role_bit(role)) != 0;
}

WorkerIndex current_worker_index() noexcept {
    return t_worker;
}

bool require_thread_role(ThreadRole required, const char* what) noexcept {
    if ((t_role_mask & role_bit(required)) != 0) {
        return true;
    }
    // Relaxed on all three: they are a report, read after the fact by a test or a diagnostic, and
    // nothing downstream of them is ordered against other memory.
    g_violations.fetch_add(1, std::memory_order_relaxed);
    g_last_violation.store(what != nullptr ? what : "", std::memory_order_relaxed);
    g_last_required.store(required, std::memory_order_relaxed);
    return false;
}

u64 thread_role_violations() noexcept {
    return g_violations.load(std::memory_order_relaxed);
}

const char* last_thread_role_violation() noexcept {
    return g_last_violation.load(std::memory_order_relaxed);
}

ThreadRole last_required_thread_role() noexcept {
    return g_last_required.load(std::memory_order_relaxed);
}

void reset_thread_role_violations() noexcept {
    g_violations.store(0, std::memory_order_relaxed);
    g_last_violation.store("", std::memory_order_relaxed);
    g_last_required.store(ThreadRole::Unknown, std::memory_order_relaxed);
}

}  // namespace cy::jobs
