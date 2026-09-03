// The breadcrumb ring: bounded, always resident, and readable from a signal handler.
//
// A fixed array and two relaxed counters. No allocation, no lock, no drain: the ring holds the last
// kBreadcrumbCapacity markers for as long as the process lives, which is what "survive into the
// crash artefact even when the trace tail is lost" requires. A reader may observe a slot being
// overwritten; it sees that as a sequence number out of order, which is honest, rather than as a
// torn value, because a slot is published with a release store after it is filled.

#include <cy/core/diagnostics/breadcrumb.h>

#include "internal.h"

#include <cy/core/diagnostics/trace.h>

#include <atomic>

namespace cy::diag {
namespace {

struct Slot {
    std::atomic<u64> published{0};  // the sequence number, or zero while never written
    u64 timestamp_ns = 0;
    u64 detail = 0;
    NameId phase = kInvalidName;
};

Slot g_slots[kBreadcrumbCapacity];
std::atomic<u64> g_sequence{0};

CY_TRACE_CATEGORY(breadcrumb_category, "breadcrumb")

}  // namespace

void breadcrumb(NameId phase, u64 detail) noexcept {
    const u64 sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    Slot& slot = g_slots[sequence % kBreadcrumbCapacity];
    slot.timestamp_ns = monotonic_now_ns();
    slot.detail = detail;
    slot.phase = phase;
    slot.published.store(sequence, std::memory_order_release);

    // Also on the timeline, on the channel the loss policy protects, so a capture that survives
    // shows the same markers in context.
    trace_emit(EventKind::Breadcrumb, Channel::Critical, phase, breadcrumb_category(), detail,
               sequence, nullptr, 0);
}

u32 breadcrumb_snapshot(Breadcrumb* out, u32 capacity) noexcept {
    if (out == nullptr || capacity == 0) {
        return 0;
    }
    const u64 latest = g_sequence.load(std::memory_order_acquire);
    const u64 oldest = (latest > kBreadcrumbCapacity) ? (latest - kBreadcrumbCapacity) + 1 : 1;
    u32 written = 0;
    for (u64 sequence = oldest; sequence <= latest && written < capacity; ++sequence) {
        const Slot& slot = g_slots[sequence % kBreadcrumbCapacity];
        if (slot.published.load(std::memory_order_acquire) != sequence) {
            continue;  // overwritten while we read, or never written
        }
        out[written] = Breadcrumb{sequence, slot.timestamp_ns, slot.detail, slot.phase};
        ++written;
    }
    return written;
}

}  // namespace cy::diag
