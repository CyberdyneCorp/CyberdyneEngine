// The rolling buffer, and the capture a condition triggers.
//
// `diagnostics-profiling-and-crash` — "Rolling buffer and automatic capture". Five claims, each of
// which would be false if the buffer were merely a trace with a shorter file:
//
//   1. ALWAYS ON, WRITING NOTHING. With the buffer open and records flowing, no artefact exists.
//   2. THE WINDOW BEFORE THE EVENT IS IN IT. A capture triggered at frame 40 contains what was
//      recorded at frame 1 — which is the only property that makes a hitch nobody was watching
//      diagnosable, and the one a "start the profiler and reproduce it" workflow cannot give.
//   3. AUTOMATIC AND MANUAL PRODUCE THE SAME FORMAT. Both are read back by the same reader.
//   4. A DECLARED CONDITION TRIGGERS IT. A frame over budget, and a health condition going
//      Critical, each produce an artefact with no capture call in sight.
//   5. NOTHING GROWS WITHOUT BOUND, AND OVERWRITING IS REPORTED. An arena too small for the window
//      loses records, says how many, and tells the health model.
//
// HOW TO MAKE IT FAIL:
//   * make `on_record()` return without copying                -> case 2 goes red (empty artefact);
//   * make `age_out()` evict everything rather than the window -> case 2 goes red;
//   * drop the `frame_budget_ns` comparison in note_frame      -> case 4a goes red;
//   * stop counting `records_overwritten`                      -> case 5 goes red.

#include "harness.h"
#include "trace_reader.h"

#include <cy/core/diagnostics/capture.h>
#include <cy/core/diagnostics/health.h>
#include <cy/core/diagnostics/log.h>
#include <cy/core/diagnostics/trace.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace cy::diag;

namespace {

CY_TRACE_CATEGORY(sample_category, "capture.test")
CY_TRACE_NAME(early_event, "capture.early")
CY_TRACE_NAME(late_event, "capture.late")
CY_TRACE_FIELD(sample_index, u64, cy::Privacy::Public)

bool file_exists(const char* path) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    std::fclose(file);
    return true;
}

RollingConfig deterministic_config() {
    RollingConfig config;
    config.window_ns = 60'000'000'000ULL;  // long enough that nothing ages out inside a test
    config.post_trigger_ns = 0;            // complete on the next poll
    config.arena_bytes = 256u * 1024u;
    config.consumer_thread = false;
    config.directory = ".";
    config.build_identity = "capture-test";
    config.cooldown_ns = 0;
    config.max_captures = 8;
    return config;
}

void emit(NameId name, u64 index) {
    const FieldValue fields[] = {field_u64(sample_index(), index)};
    trace_instant(name, sample_category(), Channel::Important, fields, 1);
}

void case_1_and_2_always_on_and_the_window_before_the_event() {
    health_reset();
    RollingConfig config = deterministic_config();
    config.frame_budget_ns = 1'000'000ULL;  // one millisecond
    CY_CHECK(rolling_open(config).has_value(), "the rolling buffer opens");

    // The window BEFORE the event: forty records nobody asked to keep.
    for (u64 index = 0; index < 40; ++index) {
        emit(early_event(), index);
    }
    trace_flush();

    const RollingStats held = rolling_stats();
    CY_CHECK(held.records_held >= 40, "the buffer holds what was emitted");
    CY_CHECK_EQ(held.captures_written, 0u, "and has written nothing, because nothing asked");
    CY_CHECK(last_capture_path()[0] == '\0', "so there is no artefact yet");

    // A declared condition: this frame took ten milliseconds against a one-millisecond budget.
    rolling_note_frame(41, 10'000'000ULL);
    emit(late_event(), 41);
    rolling_poll();

    const RollingStats after = rolling_stats();
    CY_CHECK_EQ(after.captures_written, 1u, "the overrun wrote a capture with no capture call");
    const std::string path = last_capture_path();
    CY_CHECK(!path.empty(), "and the artefact has a path");
    CY_CHECK(file_exists(path.c_str()), "and the artefact exists");

    const cy_test::Capture capture = cy_test::read_capture(path.c_str());
    CY_CHECK(capture.valid, "the artefact is readable by the ordinary reader");

    u32 early = 0;
    u32 late = 0;
    bool saw_trigger = false;
    for (const auto& record : capture.records) {
        const std::string& name = capture.name_of(record.name);
        if (name == "capture.early") {
            ++early;
        } else if (name == "capture.late") {
            ++late;
        } else if (name == "capture.triggered") {
            saw_trigger = true;
        }
    }
    // THE CLAIM. The records are from before the trigger; nobody was profiling when they happened.
    CY_CHECK_EQ(early, 40u, "every record from before the trigger is in the capture");
    CY_CHECK_EQ(late, 1u, "and the record from after it as well");
    CY_CHECK(saw_trigger, "and the artefact carries the record of its own reason");

    rolling_close();
    CY_CHECK(!rolling_is_open(), "the buffer closes");
}

void case_3_manual_capture_is_the_same_format() {
    health_reset();
    RollingConfig config = deterministic_config();
    CY_CHECK(rolling_open(config).has_value(), "the rolling buffer opens");
    for (u64 index = 0; index < 10; ++index) {
        emit(early_event(), index);
    }
    trace_flush();
    CY_CHECK(capture_trigger(CaptureTrigger::Manual, 0).has_value(),
             "a manual capture is accepted");
    rolling_poll();

    const std::string path = last_capture_path();
    CY_CHECK(path.find("manual") != std::string::npos, "the file names what asked for it");
    const cy_test::Capture capture = cy_test::read_capture(path.c_str());
    CY_CHECK(capture.valid, "manual and automatic produce one format and one reader");
    CY_CHECK(!capture.records.empty(), "and the manual capture holds the window too");
    rolling_close();
}

void case_4_health_critical_triggers_a_capture() {
    health_reset();
    RollingConfig config = deterministic_config();
    config.on_health_critical = true;
    CY_CHECK(rolling_open(config).has_value(), "the rolling buffer opens");
    emit(early_event(), 1);
    trace_flush();

    // Nothing here mentions capture. A subsystem reports a condition; the artefact appears.
    health_report(HealthCondition::DeterminismDivergence, HealthSeverity::Critical, 7);
    rolling_poll();

    const RollingStats stats = rolling_stats();
    CY_CHECK_EQ(stats.captures_written, 1u, "a health transition to Critical wrote a capture");
    CY_CHECK(std::string(last_capture_path()).find("health") != std::string::npos,
             "and the artefact names the condition kind that asked for it");

    const HealthSnapshot snapshot = health_snapshot();
    CY_CHECK(snapshot.worst == HealthSeverity::Critical, "the model reports the worst level");
    CY_CHECK_EQ(snapshot.active, 1u, "and how many conditions are active");
    const auto& entry =
        snapshot.conditions[static_cast<cy::diag::u32>(HealthCondition::DeterminismDivergence)];
    CY_CHECK(entry.since_ns != 0, "and since when — the half a level alone does not carry");
    CY_CHECK_EQ(entry.detail, 7u, "and the subsystem's own number");
    rolling_close();
}

void case_5_the_arena_is_bounded_and_says_what_it_lost() {
    health_reset();
    RollingConfig config = deterministic_config();
    config.arena_bytes = 64u * 1024u;  // deliberately far too small for what follows
    CY_CHECK(rolling_open(config).has_value(), "the rolling buffer opens");

    for (u64 index = 0; index < 20000; ++index) {
        emit(early_event(), index);
        if ((index % 500) == 0) {
            trace_flush();
        }
    }
    trace_flush();

    const RollingStats stats = rolling_stats();
    CY_CHECK(stats.bytes_held <= config.arena_bytes, "the arena never exceeds what was declared");
    CY_CHECK(stats.records_overwritten > 0,
             "an arena too small for the window loses records, and counts them");
    rolling_close();
}

/// HARD RULE: teardown under load. Producers on four threads, the background consumer draining, and
/// captures triggering while `rolling_close()` runs.
void case_6_teardown_under_load() {
    health_reset();
    RollingConfig config = deterministic_config();
    config.consumer_thread = true;
    config.drain_interval_ms = 1;
    config.arena_bytes = 512u * 1024u;
    config.cooldown_ns = 0;
    config.max_captures = 64;
    CY_CHECK(rolling_open(config).has_value(), "the rolling buffer opens with a real consumer");

    std::atomic<bool> stop{false};
    std::vector<std::thread> producers;
    producers.reserve(4);
    for (int worker = 0; worker < 4; ++worker) {
        producers.emplace_back([&stop] {
            u64 index = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                emit(early_event(), index++);
            }
        });
    }
    std::thread trigger([&stop] {
        while (!stop.load(std::memory_order_relaxed)) {
            (void)capture_trigger(CaptureTrigger::Manual, 0);
            rolling_poll();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    // Close while every one of them is still running. Nothing may hang, crash, or corrupt.
    rolling_close();
    stop.store(true, std::memory_order_relaxed);
    for (auto& producer : producers) {
        producer.join();
    }
    trigger.join();

    CY_CHECK(!rolling_is_open(), "the buffer is closed");
    CY_CHECK(!trace_is_open(), "and so is the trace it opened");
    // The producers kept emitting after the close: that must be a no-op, not a fault.
    const RollingStats stats = rolling_stats();
    CY_CHECK_EQ(stats.records_held, 0u, "and it holds nothing");
}

}  // namespace

int main() {
    case_1_and_2_always_on_and_the_window_before_the_event();
    case_3_manual_capture_is_the_same_format();
    case_4_health_critical_triggers_a_capture();
    case_5_the_arena_is_bounded_and_says_what_it_lost();
    case_6_teardown_under_load();
    return cy_test::summarise("test_capture");
}
