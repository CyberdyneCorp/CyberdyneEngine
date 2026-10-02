// Buffering and loss: bounded buffers, drops by priority, and a capture that says what it lost.
//
// `diagnostics-profiling-and-crash` — "Buffering and loss policy". The scenarios: under pressure
// the verbose channels are dropped, the essential events survive, and the loss is reported; and
// with no consumer draining, producers continue at bounded cost rather than blocking. "Silent event
// loss with no record that events were lost" is one of the forbidden patterns, and this is the test
// that it is not happening.
//
// HOW TO MAKE IT FAIL:
//   * stop recording a channel's drops in drain_slot() (trace.cpp)   -> both cases go red;
//   * admit the verbose channel up to the whole buffer (ring.h)      -> the flood case goes red,
//     because the critical records it requires no longer have room.

#include "trace_reader.h"

#include <cy/core/diagnostics/breadcrumb.h>
#include <cy/core/diagnostics/trace.h>
#include <cy/test/test.h>

#include <cstring>

using namespace cy::diag;

namespace {

CY_TRACE_CATEGORY(flood_category, "flood")
CY_TRACE_NAME(verbose_event, "flood.verbose")
CY_TRACE_NAME(sampled_event, "flood.sampled")
CY_TRACE_NAME(important_event, "flood.important")
CY_TRACE_NAME(critical_event, "flood.critical")
CY_TRACE_NAME(task_event, "flood.task")

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

/// What the artefact's LOSS chunk reports for one channel, over every thread.
u64 reported_loss(const cy_test::Capture& capture, Channel channel) {
    u64 total = 0;
    for (const auto& loss : capture.losses) {
        if (loss.channel == static_cast<u8>(channel) &&
            loss.reason == static_cast<u8>(format::LossReason::BufferPressure)) {
            total += loss.count;
        }
    }
    return total;
}

/// A small ring and no consumer: the producer is guaranteed to outrun the drain, which is the
/// condition the loss policy exists for.
TraceConfig pressured(const char* path) {
    TraceConfig config;
    config.path = path;
    config.buffer_bytes_per_thread = 4096;
    config.consumer_thread = false;
    return config;
}

}  // namespace

CY_TEST_CASE("loss: verbose records are dropped under pressure, and the loss is recorded") {
    constexpr const char* kPath = "cy_diag_loss.cytrace";
    CY_REQUIRE_MESSAGE(trace_open(pressured(kPath)).has_value(), "the trace opens");

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
    CY_REQUIRE_MESSAGE(closed.has_value(), "the trace closes");
    const TraceStats stats = closed.value();

    CY_CHECK_MESSAGE(stats.dropped[static_cast<u32>(Channel::Verbose)] > 0,
                     "verbose records were dropped under pressure");
    CY_CHECK_MESSAGE(stats.dropped[static_cast<u32>(Channel::Critical)] == 0u,
                     "no critical record was dropped");
    CY_CHECK_MESSAGE(stats.events_written < kVerboseEmissions,
                     "the buffer is bounded: not everything fits");

    const cy_test::Capture capture = cy_test::read_capture(kPath);
    CY_REQUIRE_MESSAGE(capture.valid, "the capture parses");
    CY_CHECK_MESSAGE(!capture.losses.empty(), "the artefact carries a loss record");

    u64 reported = 0;
    for (const auto& loss : capture.losses) {
        reported += loss.count;
    }
    CY_CHECK_MESSAGE(reported > 0, "the loss chunk names how much was lost");
    CY_CHECK_MESSAGE(count_kind(capture, EventKind::Loss) > 0,
                     "the loss is on the timeline too, where the gap is");
    CY_CHECK_MESSAGE(count_named(capture, "flood.critical") == kCriticalEmissions,
                     "every critical record survived the pressure the verbose ones caused");

    // Nothing blocked: the whole flood ran on this thread with no consumer at all.
    CY_CHECK_MESSAGE(stats.events_emitted > 0, "producers continued at bounded cost");
}

CY_TEST_CASE(
    "loss: a flood on every channel keeps breadcrumbs, ticks and task lifecycle, and "
    "counts what it dropped") {
    constexpr const char* kPath = "cy_diag_loss_flood.cytrace";
    CY_REQUIRE_MESSAGE(trace_open(pressured(kPath)).has_value(), "the trace opens");

    // Each round floods all four channels far past their shares of a 4 KiB ring, THEN records the
    // three essential kinds the specification names, and only then drains. So every essential
    // record is emitted into a ring the flood has already filled as far as the policy allows.
    constexpr u32 kRounds = 64;
    constexpr u32 kVerbosePerRound = 400;
    constexpr u32 kSampledPerRound = 400;
    constexpr u32 kImportantPerRound = 200;
    for (u32 round = 0; round < kRounds; ++round) {
        for (u32 index = 0; index < kVerbosePerRound; ++index) {
            trace_instant(verbose_event(), flood_category(), Channel::Verbose);
        }
        for (u32 index = 0; index < kSampledPerRound; ++index) {
            trace_instant(sampled_event(), flood_category(), Channel::Sampled);
        }
        for (u32 index = 0; index < kImportantPerRound; ++index) {
            trace_instant(important_event(), flood_category(), Channel::Important);
        }
        CY_BREADCRUMB("flood.phase", round);
        trace_tick_begin(round);
        trace_emit(EventKind::TaskBegin, Channel::Critical, task_event(), flood_category(), round,
                   0, nullptr, 0);
        trace_emit(EventKind::TaskEnd, Channel::Critical, task_event(), flood_category(), round, 0,
                   nullptr, 0);
        trace_tick_end(round);
        trace_flush();
    }

    const auto closed = trace_close();
    CY_REQUIRE_MESSAGE(closed.has_value(), "the trace closes");
    const TraceStats stats = closed.value();
    const cy_test::Capture capture = cy_test::read_capture(kPath);
    CY_REQUIRE_MESSAGE(capture.valid, "the capture parses");

    // Everything essential survived, every round.
    CY_CHECK_MESSAGE(count_kind(capture, EventKind::Breadcrumb) == kRounds,
                     "every breadcrumb survived the flood");
    CY_CHECK_MESSAGE(count_kind(capture, EventKind::TickBegin) == kRounds,
                     "every tick began on the timeline");
    CY_CHECK_MESSAGE(count_kind(capture, EventKind::TickEnd) == kRounds, "and ended on it");
    CY_CHECK_MESSAGE(count_kind(capture, EventKind::TaskBegin) == kRounds,
                     "every task's start survived");
    CY_CHECK_MESSAGE(count_kind(capture, EventKind::TaskEnd) == kRounds, "and its end");
    CY_CHECK_MESSAGE(stats.dropped[static_cast<u32>(Channel::Critical)] == 0u,
                     "nothing critical was refused");

    // What was dropped was the verbose and sampled flood, and every dropped record is accounted
    // for: what reached the artefact plus what the artefact says it lost is what was emitted.
    const u64 verbose_dropped = stats.dropped[static_cast<u32>(Channel::Verbose)];
    const u64 sampled_dropped = stats.dropped[static_cast<u32>(Channel::Sampled)];
    CY_CHECK_MESSAGE(verbose_dropped > 0, "verbose records were dropped under pressure");
    CY_CHECK_MESSAGE(sampled_dropped > 0, "and sampled ones");
    CY_CHECK_MESSAGE(reported_loss(capture, Channel::Verbose) == verbose_dropped,
                     "the artefact's loss chunk counts every verbose drop");
    CY_CHECK_MESSAGE(reported_loss(capture, Channel::Sampled) == sampled_dropped,
                     "and every sampled drop");
    CY_CHECK_MESSAGE(
        count_named(capture, "flood.verbose") + verbose_dropped == u64{kRounds} * kVerbosePerRound,
        "written plus dropped is emitted: no verbose record vanished uncounted");
    CY_CHECK_MESSAGE(
        count_named(capture, "flood.sampled") + sampled_dropped == u64{kRounds} * kSampledPerRound,
        "and no sampled record either");
}

CY_TEST_CASE("loss: a second trace in one process reports only its own losses and its own chunks") {
    // The writer belongs to the process and outlives each trace. Until issue #90 found it, a
    // capture written after another one carried the earlier capture's drops in its LOSS chunk and
    // the earlier file's chunk offsets in its index.
    constexpr const char* kFirst = "cy_diag_loss_first.cytrace";
    constexpr const char* kSecond = "cy_diag_loss_second.cytrace";
    CY_REQUIRE_MESSAGE(trace_open(pressured(kFirst)).has_value(), "the first trace opens");
    for (u32 index = 0; index < kVerboseEmissions; ++index) {
        trace_instant(verbose_event(), flood_category(), Channel::Verbose);
    }
    trace_flush();
    const auto first = trace_close();
    CY_REQUIRE_MESSAGE(first.has_value(), "the first trace closes");
    CY_REQUIRE_MESSAGE(first.value().dropped[static_cast<u32>(Channel::Verbose)] > 0,
                       "the first trace lost records");

    // The second trace is under no pressure at all: a handful of records, drained at once.
    CY_REQUIRE_MESSAGE(trace_open(pressured(kSecond)).has_value(), "the second trace opens");
    for (u32 index = 0; index < kCriticalEmissions; ++index) {
        trace_instant(critical_event(), flood_category(), Channel::Critical);
    }
    trace_flush();
    const auto second = trace_close();
    CY_REQUIRE_MESSAGE(second.has_value(), "the second trace closes");

    const cy_test::Capture capture = cy_test::read_capture(kSecond);
    CY_REQUIRE_MESSAGE(capture.valid, "the second capture parses");
    CY_CHECK_MESSAGE(reported_loss(capture, Channel::Verbose) == 0u,
                     "the second capture reports none of the first one's drops");
    CY_CHECK_MESSAGE(count_named(capture, "flood.critical") == kCriticalEmissions,
                     "and holds its own records");

    // Its index names exactly its own chunks: every entry points at a chunk header in THIS file
    // with the tag and size it claims, and there is one entry per chunk before the index itself.
    CY_CHECK_MESSAGE(capture.index.size() + 1 == capture.chunk_count,
                     "one index entry per chunk the capture holds");
    bool every_entry_is_a_chunk_here = true;
    for (const cy_test::IndexRef& entry : capture.index) {
        format::ChunkHeader header{};
        if (entry.offset + sizeof(header) > capture.bytes.size()) {
            every_entry_is_a_chunk_here = false;
            continue;
        }
        std::memcpy(&header, capture.bytes.data() + entry.offset, sizeof(header));
        every_entry_is_a_chunk_here = every_entry_is_a_chunk_here && header.tag == entry.tag &&
                                      header.payload_bytes == entry.payload_bytes;
    }
    CY_CHECK_MESSAGE(every_entry_is_a_chunk_here, "and every entry is a chunk of this file");
}
