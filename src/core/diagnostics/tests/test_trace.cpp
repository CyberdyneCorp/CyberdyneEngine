// One trace, many producers: identity, the shared timeline, and offline readability.
//
// `diagnostics-profiling-and-crash` — "One trace, many producers" and "Trace identity and
// formatting". The scenarios: a streaming stall, a task stall and a memory spike appear on one
// timeline with one clock because they were recorded through one transport; a capture opened
// without the game resolves its identifiers from its own metadata; and emission does no allocation,
// which is what "identifiers are compiled, not formatted" costs at run time.
//
// HOW TO MAKE IT FAIL:
//   * write the NAME table without its strings in writer.cpp     -> the offline-resolution case;
//   * allocate anywhere on trace_emit()'s path (a std::vector, a std::string over the small-string
//     size, a formatted message)                                 -> the allocation case.

#include "trace_reader.h"

#include <cy/core/diagnostics/breadcrumb.h>
#include <cy/core/diagnostics/log.h>
#include <cy/core/diagnostics/trace.h>
#include <cy/test/test.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

// --- Allocation counting -------------------------------------------------------------------------
//
// The global allocation functions of this binary, replaced so that a case can count what one thread
// allocated over a window. Counting is per thread and off by default, so doctest, the reader and
// the other cases allocate as they like and only the window the allocation case opens is measured.
// The aligned overloads are not replaced: nothing on the emission path asks for over-alignment, and
// their portable spelling differs per C runtime.

namespace {

std::atomic<unsigned long long> g_counted_allocations{0};
thread_local bool t_counting_allocations = false;

void* counted_allocation(std::size_t size) {
    if (t_counting_allocations) {
        g_counted_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) {
        std::abort();
    }
    return memory;
}

/// Counts the allocations this thread makes between construction and `stop()`.
class AllocationWindow {
public:
    AllocationWindow() noexcept {
        g_counted_allocations.store(0, std::memory_order_relaxed);
        t_counting_allocations = true;
    }
    ~AllocationWindow() { t_counting_allocations = false; }

    AllocationWindow(const AllocationWindow&) = delete;
    AllocationWindow& operator=(const AllocationWindow&) = delete;

    unsigned long long stop() noexcept {
        t_counting_allocations = false;
        return g_counted_allocations.load(std::memory_order_relaxed);
    }
};

}  // namespace

void* operator new(std::size_t size) {
    return counted_allocation(size);
}
void* operator new[](std::size_t size) {
    return counted_allocation(size);
}
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete[](void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}
void operator delete[](void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}

using namespace cy::diag;

namespace {

CY_TRACE_CATEGORY(streaming, "streaming")
CY_TRACE_CATEGORY(tasks, "tasks")
CY_TRACE_CATEGORY(memory, "memory")
CY_TRACE_NAME(stall_event, "streaming.stall")
CY_TRACE_NAME(task_event, "task.stall")
CY_TRACE_NAME(spike_event, "memory.spike")
CY_TRACE_FIELD(bytes_resident, u64, cy::Privacy::Public)
CY_TRACE_FIELD(asset_label, string, cy::Privacy::Public)
CY_LOG_CATEGORY(emission_log, "emission")

constexpr const char* kPath = "cy_diag_trace.cytrace";
constexpr const char* kAllocationPath = "cy_diag_trace_allocation.cytrace";

/// Three subsystems, three threads, one transport. Nothing here coordinates with anything else.
void emit_from_three_subsystems() {
    std::vector<std::thread> workers;
    workers.reserve(3);
    for (u32 index = 0; index < 3; ++index) {
        workers.emplace_back([index] {
            for (u32 iteration = 0; iteration < 32; ++iteration) {
                CY_TRACE_SCOPE("worker.iteration", tasks(), Channel::Verbose);
                trace_instant(stall_event(), streaming(), Channel::Important);
                trace_counter(spike_event(), memory(), Channel::Important,
                              (index * 1000) + iteration);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
}

/// One recorded session, shared by the cases that each read one property of it. Recorded on first
/// use rather than by whichever case happens to run first, so any one case can be run alone.
struct Recording {
    bool open_before = true;
    bool opened = false;
    TraceId id = 0;
    bool second_refused = false;
    ErrorCode second_code = ErrorCode::None;
    u64 last_frame = 0;
    u64 live_events = 0;
    bool closed = false;
    bool open_after = true;
    TraceStats stats{};
    cy_test::Capture capture;
};

Recording record_session() {
    Recording out;
    // Emission with no trace open is a no-op, not a fault. Every subsystem calls it
    // unconditionally.
    trace_instant(stall_event(), streaming(), Channel::Verbose);
    out.open_before = trace_is_open();

    TraceConfig config;
    config.path = kPath;
    config.consumer_thread = true;
    config.drain_interval_ms = 2;
    config.build_identity = "test-build";

    const auto opened = trace_open(config);
    out.opened = opened.has_value();
    out.id = opened.has_value() ? opened.value() : 0;

    const auto second = trace_open(config);
    out.second_refused = !second.has_value();
    out.second_code = second.has_value() ? ErrorCode::None : second.error().code;

    trace_frame_begin(1);
    trace_tick_begin(1);
    CY_BREADCRUMB("tick.begin", 1);
    trace_flow_begin(task_event(), tasks(), 99);
    emit_from_three_subsystems();
    trace_flow_end(task_event(), tasks(), 99);
    const FieldValue fields[] = {field_u64(bytes_resident(), 4096)};
    trace_instant(spike_event(), memory(), Channel::Important, fields, 1);
    trace_state_hash(stall_event(), 0xDEADBEEFu);
    trace_tick_end(1);
    trace_frame_end(1);
    out.last_frame = trace_last_frame();
    out.live_events = trace_stats().events_emitted;

    const auto closed = trace_close();
    out.closed = closed.has_value();
    if (closed.has_value()) {
        out.stats = closed.value();
    }
    // And emission after close is a no-op again.
    trace_instant(stall_event(), streaming(), Channel::Verbose);
    out.open_after = trace_is_open();
    out.capture = cy_test::read_capture(kPath);
    return out;
}

const Recording& recording() {
    static const Recording session = record_session();
    return session;
}

bool has_kind(const cy_test::Capture& capture, EventKind kind) {
    return std::ranges::any_of(capture.records, [kind](const cy_test::ReadRecord& record) {
        return static_cast<EventKind>(record.kind) == kind;
    });
}

/// Every shape of emission a subsystem has, once. Called once to warm the thread's buffer and the
/// static registrations the macros make on first use, and then inside the counted window.
void emit_one_of_each(u64 index) {
    static const char kLabel[] = "terrain/cell_0042";
    const FieldValue fields[] = {
        field_u64(bytes_resident(), index),
        field_text(asset_label(), kLabel, static_cast<u32>(sizeof(kLabel) - 1)),
    };
    trace_instant(stall_event(), streaming(), Channel::Important, fields, 2);
    trace_counter(spike_event(), memory(), Channel::Important, index);
    trace_scope_begin(task_event(), tasks(), Channel::Important);
    trace_scope_end(task_event(), tasks(), Channel::Important);
    trace_flow_begin(task_event(), tasks(), index);
    trace_flow_end(task_event(), tasks(), index);
    trace_tick_begin(index);
    trace_tick_end(index);
    CY_BREADCRUMB("emission.breadcrumb", index);
    CY_LOG(emission_log(), LogLevel::Warning, "emission.log", field_u64(bytes_resident(), index));
}

}  // namespace

CY_TEST_CASE("trace: emission with no trace open is a no-op, before the trace and after it") {
    const Recording& session = recording();
    CY_CHECK_MESSAGE(!session.open_before, "no trace is open before one is opened");
    CY_CHECK_MESSAGE(!session.open_after, "the trace is closed, and emitting after it is harmless");
}

CY_TEST_CASE("trace: there is one trace, and a second open is refused with its reason") {
    const Recording& session = recording();
    CY_CHECK_MESSAGE(session.opened, "the trace opens");
    CY_CHECK_MESSAGE(session.id != 0, "the trace has an identity");
    CY_CHECK_MESSAGE(session.second_refused, "a second trace cannot be opened over the first");
    CY_CHECK_MESSAGE(session.second_code == ErrorCode::AlreadyExists, "and it says why");
}

CY_TEST_CASE("trace: statistics are readable while the trace is open and when it closes") {
    const Recording& session = recording();
    CY_CHECK_MESSAGE(session.last_frame == 1u, "the last frame is readable for the crash report");
    CY_CHECK_MESSAGE(session.live_events > 0, "stats are readable while the trace is open");
    CY_CHECK_MESSAGE(session.closed, "the trace closes");
    CY_CHECK_MESSAGE(session.stats.events_emitted > 100, "every producer's records were emitted");
    // Slots are per producer, and a slot is reused once the thread that held it has ended, so the
    // count is "more than one buffer existed", not "one per thread ever created".
    CY_CHECK_MESSAGE(session.stats.threads >= 2, "producers got their own buffers");
    CY_CHECK_MESSAGE(session.stats.bytes_written > 0, "the artefact has content");
}

CY_TEST_CASE("trace: every kind of record lands on the one timeline") {
    const cy_test::Capture& capture = recording().capture;
    CY_REQUIRE_MESSAGE(capture.valid, "the capture parses without the engine");
    CY_CHECK_MESSAGE(capture.header.format_version == 1u, "the format version is recorded");
    CY_CHECK_MESSAGE(capture.records.size() > 100, "the records survived the round trip");
    CY_CHECK_MESSAGE(has_kind(capture, EventKind::ScopeBegin), "scopes are on the timeline");
    CY_CHECK_MESSAGE(has_kind(capture, EventKind::ScopeEnd), "both ends of them");
    CY_CHECK_MESSAGE(has_kind(capture, EventKind::Counter), "counters are on the timeline");
    CY_CHECK_MESSAGE(has_kind(capture, EventKind::Instant), "instants are on the timeline");
    CY_CHECK_MESSAGE(has_kind(capture, EventKind::TickBegin), "tick boundaries are on it");
    CY_CHECK_MESSAGE(has_kind(capture, EventKind::FrameBegin), "frame boundaries are on it");
    CY_CHECK_MESSAGE(has_kind(capture, EventKind::StateHash), "state hashes are on it");
    CY_CHECK_MESSAGE(has_kind(capture, EventKind::FlowBegin), "flows are on it");
    CY_CHECK_MESSAGE(has_kind(capture, EventKind::Breadcrumb), "breadcrumbs are on it");

    std::vector<u32> producers;
    for (const auto& record : capture.records) {
        if (std::ranges::find(producers, record.thread) == producers.end()) {
            producers.push_back(record.thread);
        }
    }
    CY_CHECK_MESSAGE(producers.size() >= 2,
                     "the artefact carries more than one producer's records");
}

CY_TEST_CASE("trace: identifiers resolve offline from the capture's own name table") {
    const cy_test::Capture& capture = recording().capture;
    CY_REQUIRE_MESSAGE(capture.valid, "the capture parses without the engine");
    // A viewer needs no running process: the names come from the artefact, not from this one.
    CY_CHECK_MESSAGE(!capture.names.empty(), "the capture carries its name table");
    const bool resolved = std::ranges::any_of(capture.records, [&capture](const auto& record) {
        return capture.name_of(record.name) == "streaming.stall";
    });
    CY_CHECK_MESSAGE(resolved, "an event's identifier resolves to its name offline");
    CY_CHECK_MESSAGE(capture.identity.count("engine_version") == 1,
                     "the capture identifies its build");
    const auto build = capture.identity.find("build_identity");
    CY_CHECK_MESSAGE((build != capture.identity.end() && build->second == "test-build"),
                     "and carries the identity the caller supplied");
}

CY_TEST_CASE("trace: timestamps are monotonic along each producer's records") {
    const cy_test::Capture& capture = recording().capture;
    CY_REQUIRE_MESSAGE(capture.valid, "the capture parses without the engine");
    bool ordered = true;
    u64 previous = 0;
    u32 thread = capture.records.empty() ? 0 : capture.records.front().thread;
    for (const auto& record : capture.records) {
        if (record.thread != thread) {
            thread = record.thread;
            previous = 0;
        }
        ordered = ordered && record.timestamp >= previous;
        previous = record.timestamp;
    }
    CY_CHECK_MESSAGE(ordered, "timestamps are monotonic along each producer's records");
}

CY_TEST_CASE("trace: three subsystems appear on one timeline with one clock") {
    const cy_test::Capture& capture = recording().capture;
    CY_REQUIRE_MESSAGE(capture.valid, "the capture parses without the engine");
    bool saw_streaming = false;
    bool saw_tasks = false;
    bool saw_memory = false;
    for (const auto& record : capture.records) {
        const auto category = capture.categories.find(record.category);
        if (category == capture.categories.end()) {
            continue;
        }
        saw_streaming = saw_streaming || category->second == "streaming";
        saw_tasks = saw_tasks || category->second == "tasks";
        saw_memory = saw_memory || category->second == "memory";
    }
    CY_CHECK_MESSAGE(saw_streaming, "the streaming subsystem is on the timeline");
    CY_CHECK_MESSAGE(saw_tasks, "and the task subsystem");
    CY_CHECK_MESSAGE(saw_memory, "and the memory subsystem");
}

CY_TEST_CASE("trace: emission allocates nothing once a producer has its buffer") {
    // The counter is live: without this, a replacement the linker did not pick up would report
    // zero for every window and the check below would pass over anything.
    {
        AllocationWindow control;
        // Through a volatile pointer, because a new-expression whose result is only deleted is one
        // an optimiser may remove — and then this control would measure nothing.
        int* volatile probe = new int(7);
        delete probe;
        CY_REQUIRE_MESSAGE(control.stop() == 1u, "the allocation counter sees an allocation");
    }

    TraceConfig config;
    config.path = kAllocationPath;
    config.consumer_thread = false;
    config.buffer_bytes_per_thread = 1u << 20;
    CY_REQUIRE_MESSAGE(trace_open(config).has_value(), "the trace opens");
    set_log_level(LogLevel::Info);

    // The first emission on a thread acquires that thread's ring, which is a one-time cost per
    // producer and is allowed to allocate. Every emission after it is the hot path.
    emit_one_of_each(0);
    trace_flush();
    const u64 before = trace_stats().events_emitted;

    // Sized to fit the important share of the SMALLEST ring this thread may hold. A thread keeps
    // the slot it first claimed across traces — slots are pooled — so if another case recorded on
    // this thread first, the ring is that trace's size, not the one configured above, and nothing
    // may drain inside the window because draining is the consumer's work and allocates.
    constexpr u64 kRounds = 50;
    AllocationWindow window;
    for (u64 index = 1; index <= kRounds; ++index) {
        emit_one_of_each(index);
    }
    const unsigned long long allocations = window.stop();

    const u64 emitted = trace_stats().events_emitted - before;
    trace_flush();
    const auto closed = trace_close();
    CY_CHECK_MESSAGE(closed.has_value(), "the trace closes");

    // Not vacuous: every shape of emission happened inside the window and reached a buffer.
    CY_CHECK_MESSAGE(emitted == kRounds * 10u, "every emission in the window reached the ring");
    CY_CHECK_MESSAGE(allocations == 0u,
                     "emission composes into the producer's own ring: no allocation, no "
                     "formatting, whatever the shape of the record");
}
