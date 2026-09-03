// Buffering and loss: bounded buffers, drops by priority, and a capture that says what it lost.
//
// `diagnostics-profiling-and-crash` — "Buffering and loss policy". The scenarios: under pressure
// the verbose channels are dropped, the essential events survive, and the loss is reported; and
// with no consumer draining, producers continue at bounded cost rather than blocking. "Silent event
// loss with no record that events were lost" is one of the forbidden patterns, and this is the test
// that it is not happening.

#include "harness.h"
#include "trace_reader.h"

#include <cy/core/diagnostics/trace.h>

using namespace cy::diag;

namespace {

CY_TRACE_CATEGORY(flood_category, "flood")
CY_TRACE_NAME(verbose_event, "flood.verbose")
CY_TRACE_NAME(critical_event, "flood.critical")

constexpr const char* kPath = "cy_diag_loss.cytrace";
constexpr u32 kVerboseEmissions = 20000;
constexpr u32 kCriticalEmissions = 16;

u32 count_kind(const cy_test::Capture& capture, EventKind kind) {
    u32 count = 0;
    for (const auto& record : capture.records) {
        if (static_cast<EventKind>(record.kind) == kind) {
            ++count;
        }
    }
    return count;
}

u32 count_named(const cy_test::Capture& capture, const char* name) {
    u32 count = 0;
    for (const auto& record : capture.records) {
        if (capture.name_of(record.name) == name) {
            ++count;
        }
    }
    return count;
}

}  // namespace

int main() {
    TraceConfig config;
    config.path = kPath;
    // A small buffer and no consumer: the producer is guaranteed to outrun the drain, which is the
    // condition the loss policy exists for.
    config.buffer_bytes_per_thread = 4096;
    config.consumer_thread = false;

    const auto opened = trace_open(config);
    CY_CHECK(opened.has_value(), "the trace opens");

    for (u32 index = 0; index < kVerboseEmissions; ++index) {
        trace_instant(verbose_event(), flood_category(), Channel::Verbose);
    }
    // The critical channel is admitted up to the whole buffer, and these are emitted after the
    // verbose flood has already filled it past every lower channel's share.
    for (u32 index = 0; index < kCriticalEmissions; ++index) {
        trace_instant(critical_event(), flood_category(), Channel::Critical);
    }

    trace_flush();
    const auto closed = trace_close();
    CY_CHECK(closed.has_value(), "the trace closes");
    const TraceStats stats = closed.value();

    CY_CHECK(stats.dropped[static_cast<u32>(Channel::Verbose)] > 0,
             "verbose records were dropped under pressure");
    CY_CHECK_EQ(stats.dropped[static_cast<u32>(Channel::Critical)], 0u,
                "no critical record was dropped");
    CY_CHECK(stats.events_written < kVerboseEmissions,
             "the buffer is bounded: not everything fits");

    const cy_test::Capture capture = cy_test::read_capture(kPath);
    CY_CHECK(capture.valid, "the capture parses");
    CY_CHECK(!capture.losses.empty(), "the artefact carries a loss record");

    u64 reported = 0;
    for (const auto& loss : capture.losses) {
        reported += loss.count;
    }
    CY_CHECK(reported > 0, "the loss chunk names how much was lost");
    CY_CHECK(count_kind(capture, EventKind::Loss) > 0,
             "the loss is on the timeline too, where the gap is");
    CY_CHECK_EQ(count_named(capture, "flood.critical"), kCriticalEmissions,
                "every critical record survived the pressure the verbose ones caused");

    // Nothing blocked: the whole flood ran on this thread with no consumer at all.
    CY_CHECK(stats.events_emitted > 0, "producers continued at bounded cost");
    return cy_test::summarise("loss");
}
