// Structured logging on the trace's timeline.
//
// `diagnostics-profiling-and-crash` — "Structured logging". The scenarios: a query runs over typed
// fields rather than over text, and a verbose log that is not consumed formats no string. The
// second is proved by construction — CY_LOG passes identifiers and typed values, and there is no
// formatter anywhere in the emission path — and by the first check below, which shows that a
// filtered record never reaches the transport at all.

#include "harness.h"
#include "trace_reader.h"

#include <cy/core/diagnostics/log.h>
#include <cy/core/diagnostics/trace.h>

using namespace cy::diag;

namespace {

CY_LOG_CATEGORY(net_category, "net")
CY_LOG_CATEGORY(audio_category, "audio")
CY_TRACE_FIELD(peer_id, u64, cy::Privacy::Public)
CY_TRACE_FIELD(rejection_reason, u64, cy::Privacy::Public)

constexpr const char* kPath = "cy_diag_log.cytrace";

u32 count_logs(const cy_test::Capture& capture, const char* message) {
    u32 count = 0;
    for (const auto& record : capture.records) {
        if (static_cast<EventKind>(record.kind) == EventKind::Log &&
            capture.name_of(record.name) == message) {
            ++count;
        }
    }
    return count;
}

}  // namespace

int main() {
    TraceConfig config;
    config.path = kPath;
    config.consumer_thread = false;

    const auto opened = trace_open(config);
    CY_CHECK(opened.has_value(), "the trace opens");

    set_log_level(LogLevel::Info);
    CY_CHECK(!log_should_emit(net_category(), LogLevel::Debug), "a record below the floor is cut");
    CY_CHECK(log_should_emit(net_category(), LogLevel::Warning), "a record above it is not");

    CY_LOG(net_category(), LogLevel::Debug, "net.debug.dropped");
    CY_LOG(net_category(), LogLevel::Warning, "peer.rejected", field_u64(peer_id(), 17),
           field_u64(rejection_reason(), 3));
    CY_LOG(net_category(), LogLevel::Error, "peer.lost", field_u64(peer_id(), 17));

    // A per-category floor turns one subsystem down without turning everything down.
    set_category_level(audio_category(), LogLevel::Error);
    CY_LOG(audio_category(), LogLevel::Warning, "audio.underrun");
    CY_LOG(net_category(), LogLevel::Warning, "peer.retry", field_u64(peer_id(), 18));

    trace_flush();
    const auto closed = trace_close();
    CY_CHECK(closed.has_value(), "the trace closes");

    const cy_test::Capture capture = cy_test::read_capture(kPath);
    CY_CHECK(capture.valid, "the capture parses");
    CY_CHECK_EQ(count_logs(capture, "net.debug.dropped"), 0u,
                "the filtered record was not emitted");
    CY_CHECK_EQ(count_logs(capture, "audio.underrun"), 0u, "the category floor applies");
    CY_CHECK_EQ(count_logs(capture, "peer.rejected"), 1u, "the record above the floor is there");
    CY_CHECK_EQ(count_logs(capture, "peer.retry"), 1u, "and one category's floor is not another's");

    // A query over typed fields: every rejection for one participant, without parsing text.
    u32 rejections_for_peer_17 = 0;
    u32 peer_field = 0;
    for (const auto& entry : capture.fields) {
        if (entry.second.name == "peer_id") {
            peer_field = entry.first;
        }
    }
    for (const auto& record : capture.records) {
        if (static_cast<EventKind>(record.kind) != EventKind::Log) {
            continue;
        }
        for (const auto& field : record.fields) {
            if (field.field == peer_field && field.bits == 17) {
                ++rejections_for_peer_17;
            }
        }
    }
    CY_CHECK_EQ(rejections_for_peer_17, 2u, "the query runs over typed fields");

    // A log is a record on the same timeline: the severity is the record's own payload, and the
    // source location resolves through the same name table an event name does.
    bool level_and_site = false;
    for (const auto& record : capture.records) {
        if (static_cast<EventKind>(record.kind) == EventKind::Log &&
            capture.name_of(record.name) == "peer.lost") {
            level_and_site = record.a == static_cast<u64>(LogLevel::Error) &&
                             capture.name_of(static_cast<u32>(record.b)).find("test_log.cpp") !=
                                 std::string::npos;
        }
    }
    CY_CHECK(level_and_site, "a log record carries its level and its source location");
    return cy_test::summarise("log");
}
