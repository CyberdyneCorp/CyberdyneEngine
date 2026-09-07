// The four attribution axes. M7 task 3.1; see attribution.h for the argument.

#include <cy/core/memory/attribution.h>

#include <atomic>

namespace cy {
namespace {

constexpr MemoryAttribution kNothingDeclared{};

thread_local MemoryAttribution t_attribution = kNothingDeclared;

/// Handed out in order of first use. Starts at 1 so that 0 stays "not stated" on every axis,
/// including this one — a report grouping by thread must be able to say "unattributed" for a
/// header written before the ordinal existed.
std::atomic<u32> g_next_thread_ordinal{1};
thread_local u32 t_thread_ordinal = 0;

}  // namespace

const char* attribution_axis_name(AttributionAxis axis) noexcept {
    switch (axis) {
        case AttributionAxis::Type:
            return "type";
        case AttributionAxis::Thread:
            return "thread";
        case AttributionAxis::WorldCell:
            return "world-cell";
        case AttributionAxis::Asset:
            return "asset";
    }
    return "unknown";
}

const MemoryAttribution& current_attribution() noexcept {
    return t_attribution;
}

u32 current_thread_ordinal() noexcept {
    if (t_thread_ordinal == 0) {
        t_thread_ordinal = g_next_thread_ordinal.fetch_add(1, std::memory_order_relaxed);
    }
    return t_thread_ordinal;
}

u32 thread_ordinal_count() noexcept {
    return g_next_thread_ordinal.load(std::memory_order_relaxed) - 1;
}

MemoryAttributionScope::MemoryAttributionScope(const MemoryAttribution& attribution) noexcept
    : previous_(t_attribution) {
    t_attribution = attribution.merged_over(previous_);
}

MemoryAttributionScope::~MemoryAttributionScope() {
    t_attribution = previous_;
}

}  // namespace cy
