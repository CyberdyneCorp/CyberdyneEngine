// The rolling buffer's implementation: a fixed arena, a deque of slots, and one writer per capture.
//
// The whole file is a consumer. Nothing here runs on a producer's thread, nothing here is reached
// from the emission path, and the only synchronisation is one mutex the drain consumer already
// holds a sibling of — so a producer's cost is unchanged by whether a rolling buffer exists.

#include <cy/core/diagnostics/capture.h>

#include "internal.h"
#include "writer.h"

#include <cy/core/diagnostics/field.h>
#include <cy/core/diagnostics/format.h>
#include <cy/core/diagnostics/trace.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace cy::diag {
namespace {

CY_TRACE_CATEGORY(capture_category, "capture")
CY_TRACE_NAME(capture_triggered, "capture.triggered")
CY_TRACE_FIELD(capture_reason, id, cy::Privacy::Public)
CY_TRACE_FIELD(capture_detail, u64, cy::Privacy::Public)

/// One record's place in the arena. Small and trivially copyable, so the deque of them is an
/// allocation at open and nothing afterwards in steady state.
struct Slot {
    u32 thread = 0;
    u32 offset = 0;
    u32 size = 0;
    u64 timestamp_ns = 0;
};

struct Buffer {
    std::mutex mutex;
    RollingConfig config{};
    std::vector<u8> arena;
    std::deque<Slot> slots;
    u32 write_offset = 0;

    RollingStats stats{};
    std::atomic<bool> open{false};
    std::atomic<bool> pending{false};
    /// Atomic because `rolling_poll()` reads it without the mutex — the fast path with nothing
    /// pending must not take a lock, and a torn read of a deadline is still a data race.
    std::atomic<u64> pending_until_ns{0};
    CaptureTrigger pending_trigger = CaptureTrigger::Manual;
    u64 pending_detail = 0;
    u64 last_capture_ns = 0;
    u32 capture_index = 0;
    char last_path[512] = {};
    /// Guards the one re-entrant path: a trigger reports health, health notifies the observer, and
    /// the observer is this file. One flag, because a recursive capture is never what was meant.
    bool inside_trigger = false;
};

Buffer& buffer() noexcept {
    static Buffer instance;
    return instance;
}

u64 record_timestamp(const u8* record, u32 size) noexcept {
    if (size < format::kRecordFixedBytes) {
        return 0;
    }
    format::RecordBody body{};
    std::memcpy(&body, record + sizeof(format::RecordHeader), sizeof(body));
    return body.timestamp_ns;
}

/// Drop the front slot, attributing it to the reason it left. Caller holds the mutex.
void evict_front(Buffer& state, bool aged_out) noexcept {
    if (state.slots.empty()) {
        return;
    }
    state.stats.bytes_held -= state.slots.front().size;
    --state.stats.records_held;
    if (aged_out) {
        ++state.stats.records_aged_out;
    } else {
        ++state.stats.records_overwritten;
    }
    state.slots.pop_front();
}

/// Make room for `size` bytes at the write cursor, wrapping if it does not fit in the tail, and
/// evicting every slot the write would land on. Caller holds the mutex.
bool make_room(Buffer& state, u32 size) noexcept {
    const u32 capacity = static_cast<u32>(state.arena.size());
    if (size > capacity) {
        return false;  // one record larger than the whole arena; counted by the caller
    }
    if (state.write_offset + size > capacity) {
        state.write_offset = 0;
    }
    const u32 begin = state.write_offset;
    const u32 end = begin + size;
    while (!state.slots.empty()) {
        const Slot& front = state.slots.front();
        const bool overlaps = front.offset < end && begin < front.offset + front.size;
        if (!overlaps) {
            break;
        }
        evict_front(state, false);
    }
    return true;
}

/// Drop everything older than the configured window. Caller holds the mutex.
void age_out(Buffer& state, u64 newest_ns) noexcept {
    if (state.config.window_ns == 0 || newest_ns < state.config.window_ns) {
        return;
    }
    const u64 horizon = newest_ns - state.config.window_ns;
    while (!state.slots.empty() && state.slots.front().timestamp_ns < horizon) {
        evict_front(state, true);
    }
}

/// Write everything held, grouped by thread, as a capture artefact in the ordinary format.
/// Caller holds the mutex. Returns false when the file could not be written.
bool write_capture(Buffer& state, CaptureTrigger trigger) noexcept {
    ++state.capture_index;
    const char* directory = (state.config.directory != nullptr) ? state.config.directory : ".";
    std::snprintf(state.last_path, sizeof(state.last_path), "%s/capture-%s-%04u.cytrace", directory,
                  capture_trigger_name(trigger), state.capture_index);

    TraceConfig config;
    config.path = state.last_path;
    config.policy = state.config.policy;
    config.build_identity = state.config.build_identity;
    config.consumer_thread = false;

    TraceWriter writer;
    const auto opened = writer.open(config, monotonic_now_ns() ^ state.capture_index);
    if (!opened) {
        state.last_path[0] = '\0';
        return false;
    }

    // Grouped by thread because the format's EVTS chunk is one thread's records. Held order is
    // preserved within a thread, which is the order the producer wrote them in.
    std::vector<u32> threads;
    for (const Slot& slot : state.slots) {
        bool seen = false;
        for (u32 known : threads) {
            if (known == slot.thread) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            threads.push_back(slot.thread);
        }
    }
    for (u32 thread : threads) {
        writer.begin_thread_chunk(thread);
        for (const Slot& slot : state.slots) {
            if (slot.thread == thread) {
                writer.append_record(state.arena.data() + slot.offset, slot.size);
            }
        }
        writer.end_thread_chunk();
    }

    // What the buffer itself lost, in the artefact rather than only in the process: a capture whose
    // window did not fit says so where a reader looks for gaps.
    writer.record_loss(0, Channel::Critical, format::LossReason::BufferPressure,
                       state.stats.records_overwritten);
    const auto closed = writer.close();
    if (!closed) {
        state.last_path[0] = '\0';
        return false;
    }
    ++state.stats.captures_written;
    state.last_capture_ns = monotonic_now_ns();
    return true;
}

void on_record(void* /*user*/, u32 thread_index, const u8* record, u32 size) noexcept {
    Buffer& state = buffer();
    const std::lock_guard<std::mutex> guard(state.mutex);
    if (state.arena.empty()) {
        return;
    }
    if (!make_room(state, size)) {
        ++state.stats.records_overwritten;
        return;
    }
    const u32 offset = state.write_offset;
    std::memcpy(state.arena.data() + offset, record, size);
    state.write_offset = offset + size;

    const u64 timestamp = record_timestamp(record, size);
    state.slots.push_back(Slot{thread_index, offset, size, timestamp});
    ++state.stats.records_held;
    state.stats.bytes_held += size;
    state.stats.newest_ns = timestamp;
    age_out(state, timestamp);
    state.stats.oldest_ns = state.slots.empty() ? 0 : state.slots.front().timestamp_ns;
}

void on_health(void* /*user*/, HealthCondition condition, HealthSeverity /*from*/,
               HealthSeverity to) noexcept {
    if (to != HealthSeverity::Critical) {
        return;
    }
    Buffer& state = buffer();
    if (!state.open.load(std::memory_order_acquire) || !state.config.on_health_critical) {
        return;
    }
    (void)capture_trigger(CaptureTrigger::HealthCritical, static_cast<u64>(condition));
}

}  // namespace

const char* capture_trigger_name(CaptureTrigger trigger) noexcept {
    switch (trigger) {
        case CaptureTrigger::Manual:
            return "manual";
        case CaptureTrigger::FrameBudgetOverrun:
            return "frame";
        case CaptureTrigger::TickBudgetOverrun:
            return "tick";
        case CaptureTrigger::GpuBudgetOverrun:
            return "gpu";
        case CaptureTrigger::Assertion:
            return "assertion";
        case CaptureTrigger::HealthCritical:
            return "health";
    }
    return "unknown";
}

Expected<u32, cy::Error> rolling_open(const RollingConfig& config) noexcept {
    Buffer& state = buffer();
    if (state.open.load(std::memory_order_acquire)) {
        return fail(ErrorCode::AlreadyExists, "a rolling buffer is already open");
    }
    if (config.arena_bytes < 64u * 1024u) {
        return fail(ErrorCode::InvalidArgument,
                    "a rolling arena smaller than 64 KiB holds no useful window");
    }
    {
        const std::lock_guard<std::mutex> guard(state.mutex);
        state.config = config;
        state.arena.assign(config.arena_bytes, 0);
        state.slots.clear();
        state.write_offset = 0;
        state.stats = RollingStats{};
        state.capture_index = 0;
        state.last_capture_ns = 0;
        state.last_path[0] = '\0';
        state.pending_until_ns.store(0, std::memory_order_relaxed);
        state.pending.store(false, std::memory_order_relaxed);
    }

    TraceConfig trace;
    trace.path = nullptr;  // always on, and therefore writing nothing until asked
    trace.policy = config.policy;
    trace.build_identity = config.build_identity;
    trace.observer = &on_record;
    trace.observer_user = nullptr;
    trace.consumer_thread = config.consumer_thread;
    trace.drain_interval_ms = config.drain_interval_ms;
    const auto opened = trace_open(trace);
    if (!opened) {
        const std::lock_guard<std::mutex> guard(state.mutex);
        state.arena.clear();
        state.arena.shrink_to_fit();
        return fail(opened.error().code, "the rolling buffer could not open the trace");
    }
    state.open.store(true, std::memory_order_release);
    (void)set_health_observer(&on_health, nullptr);
    return 0u;
}

void rolling_close() noexcept {
    Buffer& state = buffer();
    if (!state.open.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    (void)set_health_observer(nullptr, nullptr);
    trace_flush();
    // A session that ends between a trigger and its post-window still leaves the artefact behind:
    // the window is short by whatever was not recorded, which is better than nothing at all.
    if (state.pending.exchange(false, std::memory_order_acq_rel)) {
        const std::lock_guard<std::mutex> guard(state.mutex);
        (void)write_capture(state, state.pending_trigger);
    }
    (void)trace_close();
    const std::lock_guard<std::mutex> guard(state.mutex);
    state.slots.clear();
    state.arena.clear();
    state.arena.shrink_to_fit();
    // What it HOLDS is now nothing; what it DID — captures written, records aged out or
    // overwritten, triggers suppressed — is the session's record and survives the close.
    state.stats.records_held = 0;
    state.stats.bytes_held = 0;
    state.stats.oldest_ns = 0;
    state.stats.newest_ns = 0;
}

bool rolling_is_open() noexcept {
    return buffer().open.load(std::memory_order_relaxed);
}

RollingStats rolling_stats() noexcept {
    Buffer& state = buffer();
    const std::lock_guard<std::mutex> guard(state.mutex);
    RollingStats stats = state.stats;
    stats.capture_pending = state.pending.load(std::memory_order_relaxed);
    return stats;
}

Expected<u32, cy::Error> capture_trigger(CaptureTrigger trigger, u64 detail) noexcept {
    Buffer& state = buffer();
    if (!state.open.load(std::memory_order_acquire)) {
        return fail(ErrorCode::Unavailable, "no rolling buffer is open");
    }
    {
        const std::lock_guard<std::mutex> guard(state.mutex);
        if (state.inside_trigger) {
            return fail(ErrorCode::Unavailable, "a capture is already being triggered");
        }
        if (state.pending.load(std::memory_order_relaxed)) {
            ++state.stats.triggers_suppressed;
            return fail(ErrorCode::Unavailable, "a capture is already pending");
        }
        if (state.stats.captures_written >= state.config.max_captures) {
            ++state.stats.triggers_suppressed;
            return fail(ErrorCode::Unavailable, "this session's capture budget is spent");
        }
        const u64 now = monotonic_now_ns();
        if (state.last_capture_ns != 0 && now - state.last_capture_ns < state.config.cooldown_ns) {
            ++state.stats.triggers_suppressed;
            return fail(ErrorCode::Unavailable, "the capture cooldown has not elapsed");
        }
        state.inside_trigger = true;
        state.pending_trigger = trigger;
        state.pending_detail = detail;
        state.pending_until_ns.store(now + state.config.post_trigger_ns, std::memory_order_relaxed);
        state.pending.store(true, std::memory_order_release);
    }

    // On the timeline, so the artefact contains the record of its own reason.
    const FieldValue fields[] = {
        field_u64(capture_reason(), static_cast<u64>(trigger)),
        field_u64(capture_detail(), detail),
    };
    trace_instant(capture_triggered(), capture_category(), Channel::Critical, fields, 2);

    {
        const std::lock_guard<std::mutex> guard(state.mutex);
        state.inside_trigger = false;
    }
    return state.capture_index + 1;
}

void rolling_poll() noexcept {
    Buffer& state = buffer();
    if (!state.pending.load(std::memory_order_acquire)) {
        return;
    }
    if (monotonic_now_ns() < state.pending_until_ns.load(std::memory_order_relaxed)) {
        return;
    }
    // Drain first, so the post-trigger window is actually in the buffer rather than still in the
    // producers' rings. trace_flush() re-enters on_record(), which takes the mutex, so it must
    // happen before this function takes it.
    trace_flush();
    if (!state.pending.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    const std::lock_guard<std::mutex> guard(state.mutex);
    (void)write_capture(state, state.pending_trigger);
}

const char* last_capture_path() noexcept {
    return buffer().last_path;
}

void rolling_note_frame(u64 frame_index, u64 duration_ns) noexcept {
    Buffer& state = buffer();
    if (!state.open.load(std::memory_order_acquire)) {
        return;
    }
    // The pump. A frame boundary is the natural place to complete a capture whose post-trigger
    // window has elapsed, and it means the buffer needs no thread of its own: with nothing pending
    // this is one relaxed load.
    rolling_poll();
    const u64 budget = state.config.frame_budget_ns;
    if (budget == 0) {
        return;
    }
    if (duration_ns <= budget) {
        health_report(HealthCondition::FrameBudgetOverrun, HealthSeverity::Nominal, duration_ns);
        return;
    }
    health_report(HealthCondition::FrameBudgetOverrun, HealthSeverity::Degraded, duration_ns);
    (void)capture_trigger(CaptureTrigger::FrameBudgetOverrun, frame_index);
}

void rolling_note_tick(u64 tick_index, u64 duration_ns) noexcept {
    Buffer& state = buffer();
    if (!state.open.load(std::memory_order_acquire)) {
        return;
    }
    // The pump. A frame boundary is the natural place to complete a capture whose post-trigger
    // window has elapsed, and it means the buffer needs no thread of its own: with nothing pending
    // this is one relaxed load.
    rolling_poll();
    const u64 budget = state.config.tick_budget_ns;
    if (budget == 0) {
        return;
    }
    if (duration_ns <= budget) {
        health_report(HealthCondition::TickBudgetOverrun, HealthSeverity::Nominal, duration_ns);
        return;
    }
    health_report(HealthCondition::TickBudgetOverrun, HealthSeverity::Degraded, duration_ns);
    (void)capture_trigger(CaptureTrigger::TickBudgetOverrun, tick_index);
}

void rolling_note_gpu(u64 frame_index, u64 duration_ns) noexcept {
    Buffer& state = buffer();
    if (!state.open.load(std::memory_order_acquire)) {
        return;
    }
    // The pump. A frame boundary is the natural place to complete a capture whose post-trigger
    // window has elapsed, and it means the buffer needs no thread of its own: with nothing pending
    // this is one relaxed load.
    rolling_poll();
    const u64 budget = state.config.gpu_budget_ns;
    if (budget == 0) {
        return;
    }
    if (duration_ns <= budget) {
        health_report(HealthCondition::GpuBudgetOverrun, HealthSeverity::Nominal, duration_ns);
        return;
    }
    health_report(HealthCondition::GpuBudgetOverrun, HealthSeverity::Degraded, duration_ns);
    (void)capture_trigger(CaptureTrigger::GpuBudgetOverrun, frame_index);
}

void capture_on_assertion() noexcept {
    Buffer& state = buffer();
    if (!state.open.load(std::memory_order_acquire) || !state.config.on_assertion) {
        return;
    }
    if (!capture_trigger(CaptureTrigger::Assertion, 0)) {
        return;
    }
    // No post-trigger window: the process is on its way to aborting and will not reach one.
    trace_flush();
    if (!state.pending.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    const std::lock_guard<std::mutex> guard(state.mutex);
    (void)write_capture(state, CaptureTrigger::Assertion);
}

}  // namespace cy::diag
