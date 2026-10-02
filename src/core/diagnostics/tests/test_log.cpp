// Structured logging on the trace's timeline.
//
// `diagnostics-profiling-and-crash` — "Structured logging". The scenarios: a query runs over typed
// fields rather than over text, and a verbose log that is not consumed formats no string. The
// second is proved by construction — CY_LOG passes identifiers and typed values, and there is no
// formatter anywhere in the emission path — and by the filtering case below, which shows that a
// filtered record never reaches the transport at all.
//
// HOW TO MAKE IT FAIL:
//   * drop the last field of a record in log_emit() (log.cpp)     -> the by-field query case;
//   * stop comparing against the category floor in log_should_emit -> the filtering case.

#include "trace_reader.h"

#include <cy/core/diagnostics/log.h>
#include <cy/core/diagnostics/trace.h>
#include <cy/test/test.h>

#include <set>
#include <string>

using namespace cy::diag;

namespace {

CY_LOG_CATEGORY(net_category, "net")
CY_LOG_CATEGORY(audio_category, "audio")
CY_LOG_CATEGORY(ability_category, "abilities")
CY_TRACE_FIELD(peer_id, u64, cy::Privacy::Public)
CY_TRACE_FIELD(rejection_reason, u64, cy::Privacy::Public)
CY_TRACE_FIELD(ability_id, u64, cy::Privacy::Public)
CY_TRACE_FIELD(participant_id, u64, cy::Privacy::Public)

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

/// The id the capture's own metadata gives a field, looked up by name as a tool would.
u32 field_named(const cy_test::Capture& capture, const char* name) {
    for (const auto& entry : capture.fields) {
        if (entry.second.name == name) {
            return entry.first;
        }
    }
    return 0;
}

/// The value of `field` on `record`, if the record carries it.
bool field_value(const cy_test::ReadRecord& record, u32 field, u64& out) {
    for (const auto& value : record.fields) {
        if (value.field == field) {
            out = value.bits;
            return true;
        }
    }
    return false;
}

cy_test::Capture record_session(const char* path, void (*emit)()) {
    TraceConfig config;
    config.path = path;
    config.consumer_thread = false;
    const bool opened = trace_open(config).has_value();
    CY_CHECK_MESSAGE(opened, "the trace opens");
    emit();
    trace_flush();
    CY_CHECK_MESSAGE(trace_close().has_value(), "the trace closes");
    return cy_test::read_capture(path);
}

void emit_filtered_records() {
    set_log_level(LogLevel::Info);
    CY_LOG(net_category(), LogLevel::Debug, "net.debug.dropped");
    CY_LOG(net_category(), LogLevel::Warning, "peer.rejected", field_u64(peer_id(), 17),
           field_u64(rejection_reason(), 3));
    CY_LOG(net_category(), LogLevel::Error, "peer.lost", field_u64(peer_id(), 17));

    // A per-category floor turns one subsystem down without turning everything down.
    set_category_level(audio_category(), LogLevel::Error);
    CY_LOG(audio_category(), LogLevel::Warning, "audio.underrun");
    CY_LOG(net_category(), LogLevel::Warning, "peer.retry", field_u64(peer_id(), 18));
}

/// Three participants try abilities; some activations are rejected. The fields are what a game's
/// ability system would record, and the participant is deliberately the LAST field, so a record
/// that lost a field loses the one the query selects on.
void emit_ability_activations() {
    set_log_level(LogLevel::Info);
    for (u64 participant = 1; participant <= 3; ++participant) {
        for (u64 ability = 10; ability < 14; ++ability) {
            if (((participant + ability) % 2) == 0) {
                CY_LOG(ability_category(), LogLevel::Warning, "ability.rejected",
                       field_u64(ability_id(), ability), field_u64(rejection_reason(), ability % 3),
                       field_u64(participant_id(), participant));
            } else {
                CY_LOG(ability_category(), LogLevel::Info, "ability.activated",
                       field_u64(ability_id(), ability), field_u64(participant_id(), participant));
            }
        }
    }
}

}  // namespace

CY_TEST_CASE("log: a record below the global or the category floor never reaches the trace") {
    set_log_level(LogLevel::Info);
    CY_CHECK_MESSAGE(!log_should_emit(net_category(), LogLevel::Debug),
                     "a record below the floor is cut");
    CY_CHECK_MESSAGE(log_should_emit(net_category(), LogLevel::Warning),
                     "a record above it is not");

    const cy_test::Capture capture = record_session("cy_diag_log.cytrace", emit_filtered_records);
    CY_REQUIRE_MESSAGE(capture.valid, "the capture parses");
    CY_CHECK_MESSAGE(count_logs(capture, "net.debug.dropped") == 0u,
                     "the filtered record was not emitted");
    CY_CHECK_MESSAGE(count_logs(capture, "audio.underrun") == 0u, "the category floor applies");
    CY_CHECK_MESSAGE(count_logs(capture, "peer.rejected") == 1u,
                     "the record above the floor is there");
    CY_CHECK_MESSAGE(count_logs(capture, "peer.retry") == 1u,
                     "and one category's floor is not another's");

    // A query over typed fields: every record for one participant, without parsing text.
    const u32 peer_field = field_named(capture, "peer_id");
    u32 records_for_peer_17 = 0;
    for (const auto& record : capture.records) {
        u64 peer = 0;
        if (static_cast<EventKind>(record.kind) == EventKind::Log &&
            field_value(record, peer_field, peer) && peer == 17) {
            ++records_for_peer_17;
        }
    }
    CY_CHECK_MESSAGE(records_for_peer_17 == 2u, "the query runs over typed fields");
}

CY_TEST_CASE("log: a record carries its level and its source location") {
    const cy_test::Capture capture =
        record_session("cy_diag_log_site.cytrace", emit_filtered_records);
    CY_REQUIRE_MESSAGE(capture.valid, "the capture parses");
    // A log is a record on the same timeline: the severity is the record's own payload, and the
    // source location resolves through the LOCATION table — its own, classified, sanitised table,
    // never the name table. See src/core/diagnostics/source.h and test_source_privacy.cpp.
    bool level_and_site = false;
    for (const auto& record : capture.records) {
        if (static_cast<EventKind>(record.kind) == EventKind::Log &&
            capture.name_of(record.name) == "peer.lost") {
            level_and_site = record.a == static_cast<u64>(LogLevel::Error) &&
                             capture.location_of(static_cast<u32>(record.b)).find("test_log.cpp") !=
                                 std::string::npos;
        }
    }
    CY_CHECK_MESSAGE(level_and_site, "a log record carries its level and its source location");
}

CY_TEST_CASE("log: every rejected ability activation for one participant is selected by field") {
    const cy_test::Capture capture =
        record_session("cy_diag_log_abilities.cytrace", emit_ability_activations);
    CY_REQUIRE_MESSAGE(capture.valid, "the capture parses");

    // The query a person debugging participant 2 runs: by message identifier and by typed field,
    // with no text parsed anywhere. Participant 2 tried abilities 10..13 and was refused the even
    // ones.
    const u32 participant = field_named(capture, "participant_id");
    const u32 ability = field_named(capture, "ability_id");
    const u32 reason = field_named(capture, "rejection_reason");
    CY_REQUIRE_MESSAGE(participant != 0, "the capture declares the participant field");

    std::set<u64> rejected_abilities;
    u32 rejections_with_reason = 0;
    for (const auto& record : capture.records) {
        u64 who = 0;
        if (static_cast<EventKind>(record.kind) != EventKind::Log ||
            capture.name_of(record.name) != "ability.rejected" ||
            !field_value(record, participant, who) || who != 2) {
            continue;
        }
        u64 which = 0;
        u64 why = 0;
        if (field_value(record, ability, which)) {
            rejected_abilities.insert(which);
        }
        rejections_with_reason += field_value(record, reason, why) ? 1u : 0u;
    }
    CY_CHECK_MESSAGE((rejected_abilities == std::set<u64>{10, 12}),
                     "exactly participant 2's rejected activations are selected");
    CY_CHECK_MESSAGE(rejections_with_reason == 2u, "and each one says why, as a typed field");
    CY_CHECK_MESSAGE(count_logs(capture, "ability.rejected") == 6u,
                     "the other participants' rejections are in the capture, and not selected");
}
