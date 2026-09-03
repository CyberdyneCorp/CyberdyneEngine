// One trace, many producers: identity, the shared timeline, and offline readability.
//
// `diagnostics-profiling-and-crash` — "One trace, many producers" and "Trace identity and
// formatting". The scenarios: a streaming stall, a task stall and a memory spike appear on one
// timeline with one clock because they were recorded through one transport; and a capture opened
// without the game resolves its identifiers from its own metadata.

#include "harness.h"
#include "trace_reader.h"

#include <cy/core/diagnostics/breadcrumb.h>
#include <cy/core/diagnostics/log.h>
#include <cy/core/diagnostics/trace.h>

#include <algorithm>
#include <thread>
#include <vector>

using namespace cy::diag;

namespace {

CY_TRACE_CATEGORY(streaming, "streaming")
CY_TRACE_CATEGORY(tasks, "tasks")
CY_TRACE_CATEGORY(memory, "memory")
CY_TRACE_NAME(stall_event, "streaming.stall")
CY_TRACE_NAME(task_event, "task.stall")
CY_TRACE_NAME(spike_event, "memory.spike")
CY_TRACE_NAME(worker_scope, "worker.frame")
CY_TRACE_FIELD(bytes_resident, u64, cy::Privacy::Public)

constexpr const char* kPath = "cy_diag_trace.cytrace";

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

void check_kinds_present(const cy_test::Capture& capture) {
    bool scope_begin = false;
    bool scope_end = false;
    bool counter = false;
    bool instant = false;
    bool tick = false;
    bool frame = false;
    bool state_hash = false;
    bool flow = false;
    bool crumb = false;
    for (const auto& record : capture.records) {
        switch (static_cast<EventKind>(record.kind)) {
            case EventKind::ScopeBegin:
                scope_begin = true;
                break;
            case EventKind::ScopeEnd:
                scope_end = true;
                break;
            case EventKind::Counter:
                counter = true;
                break;
            case EventKind::Instant:
                instant = true;
                break;
            case EventKind::TickBegin:
                tick = true;
                break;
            case EventKind::FrameBegin:
                frame = true;
                break;
            case EventKind::StateHash:
                state_hash = true;
                break;
            case EventKind::FlowBegin:
                flow = true;
                break;
            case EventKind::Breadcrumb:
                crumb = true;
                break;
            default:
                break;
        }
    }
    CY_CHECK(scope_begin && scope_end, "scopes are on the timeline");
    CY_CHECK(counter, "counters are on the timeline");
    CY_CHECK(instant, "instants are on the timeline");
    CY_CHECK(tick, "tick boundaries are on the timeline");
    CY_CHECK(frame, "frame boundaries are on the timeline");
    CY_CHECK(state_hash, "state hashes are on the timeline");
    CY_CHECK(flow, "flows are on the timeline");
    CY_CHECK(crumb, "breadcrumbs are on the timeline");
}

}  // namespace

int main() {
    // Emission with no trace open is a no-op, not a fault. Every subsystem calls it
    // unconditionally.
    trace_instant(stall_event(), streaming(), Channel::Verbose);
    CY_CHECK(!trace_is_open(), "no trace is open before one is opened");

    TraceConfig config;
    config.path = kPath;
    config.consumer_thread = true;
    config.drain_interval_ms = 2;
    config.build_identity = "test-build";

    const auto opened = trace_open(config);
    CY_CHECK(opened.has_value(), "the trace opens");
    CY_CHECK(opened.has_value() && opened.value() != 0, "the trace has an identity");

    const auto second = trace_open(config);
    CY_CHECK(!second.has_value(), "a second trace cannot be opened over the first");
    CY_CHECK(!second.has_value() && second.error().code == ErrorCode::AlreadyExists,
             "and it says why");

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
    CY_CHECK_EQ(trace_last_frame(), 1u, "the last frame is readable for the crash report");

    const TraceStats live = trace_stats();
    CY_CHECK(live.events_emitted > 0, "stats are readable while the trace is open");

    const auto closed = trace_close();
    CY_CHECK(closed.has_value(), "the trace closes");
    const TraceStats stats = closed.value();
    CY_CHECK(stats.events_emitted > 100, "every producer's records were emitted");
    // Slots are per producer, and a slot is reused once the thread that held it has ended, so the
    // count is "more than one buffer existed", not "one per thread ever created".
    CY_CHECK(stats.threads >= 2, "producers got their own buffers");
    CY_CHECK(stats.bytes_written > 0, "the artefact has content");

    const cy_test::Capture capture = cy_test::read_capture(kPath);
    CY_CHECK(capture.valid, "the capture parses without the engine");
    CY_CHECK_EQ(capture.header.format_version, 1u, "the format version is recorded");
    CY_CHECK(capture.records.size() > 100, "the records survived the round trip");
    check_kinds_present(capture);

    std::vector<u32> producers;
    for (const auto& record : capture.records) {
        if (std::ranges::find(producers, record.thread) == producers.end()) {
            producers.push_back(record.thread);
        }
    }
    CY_CHECK(producers.size() >= 2, "the artefact carries more than one producer's records");

    // Identifiers resolve from the capture's own metadata: a viewer needs no running process.
    CY_CHECK(!capture.names.empty(), "the capture carries its name table");
    bool resolved = false;
    for (const auto& record : capture.records) {
        if (capture.name_of(record.name) == "streaming.stall") {
            resolved = true;
        }
    }
    CY_CHECK(resolved, "an event's identifier resolves to its name offline");
    CY_CHECK(capture.identity.count("engine_version") == 1, "the capture identifies its build");
    CY_CHECK(capture.identity.find("build_identity")->second == "test-build",
             "and the identity the caller supplied");

    // One clock: every record's timestamp is monotonic within its own thread's chunk.
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
    CY_CHECK(ordered, "timestamps are monotonic along each producer's records");

    // Three subsystems, one timeline.
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
    CY_CHECK(saw_streaming && saw_tasks && saw_memory,
             "three subsystems appear on one timeline with one clock");

    // And emission after close is a no-op again.
    trace_instant(stall_event(), streaming(), Channel::Verbose);
    CY_CHECK(!trace_is_open(), "the trace is closed");
    return cy_test::summarise("trace");
}
