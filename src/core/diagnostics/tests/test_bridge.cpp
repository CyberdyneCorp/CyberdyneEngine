// The seams src/core/base/ declares, filled in by diagnostics.
//
// `diagnostics-profiling-and-crash` — "Structured logging" and "Assertions and health". base emits
// a warning through a function pointer because layer 0's lower half cannot call upward; the
// property worth testing is that once diagnostics is running, that warning is a record on the same
// timeline as everything else, carrying a classification like every other field.
//
// The assertion half of the bridge ends the process by construction — base's
// report_assertion_failure is [[noreturn]] — so it is tested by crash_probe.cpp and test_crash.py,
// which read the artefact a real assertion failure leaves behind.

#include "harness.h"
#include "trace_reader.h"

#include <cy/core/diagnostics/bridge.h>
#include <cy/core/diagnostics/log.h>
#include <cy/core/diagnostics/trace.h>

#include <cy/core/base/diagnostic_sink.h>

using namespace cy::diag;

namespace {
constexpr const char* kPath = "cy_diag_bridge.cytrace";
constexpr const char* kMessage = "vsync mode Adaptive is not supported; ignoring";
}  // namespace

int main() {
    TraceConfig config;
    config.path = kPath;
    config.consumer_thread = false;
    const auto opened = trace_open(config);
    CY_CHECK(opened.has_value(), "the trace opens");

    // What platform/ does when it refuses a feature rather than failing window creation.
    cy::emit_diagnostic(cy::DiagnosticSeverity::Warning, "display", kMessage);
    cy::emit_diagnosticf(cy::DiagnosticSeverity::Error, "display", "screen %d has no mode", 3);

    trace_flush();
    const auto closed = trace_close();
    CY_CHECK(closed.has_value(), "the trace closes");

    const cy_test::Capture capture = cy_test::read_capture(kPath);
    CY_CHECK(capture.valid, "the capture parses");

    u32 warnings = 0;
    u32 errors = 0;
    bool carries_message = false;
    bool classified = false;
    for (const auto& record : capture.records) {
        if (static_cast<EventKind>(record.kind) != EventKind::Log) {
            continue;
        }
        const auto category = capture.categories.find(record.category);
        if (category == capture.categories.end() || category->second != "display") {
            continue;
        }
        warnings += (record.a == static_cast<u64>(LogLevel::Warning)) ? 1 : 0;
        errors += (record.a == static_cast<u64>(LogLevel::Error)) ? 1 : 0;
        for (const auto& value : record.fields) {
            carries_message = carries_message || value.text == kMessage;
            const auto meta = capture.fields.find(value.field);
            classified = classified || (meta != capture.fields.end() && meta->second.privacy != 0);
        }
    }
    CY_CHECK_EQ(warnings, 1u, "the warning became a record on the timeline");
    CY_CHECK_EQ(errors, 1u, "and so did the formatted one");
    CY_CHECK(carries_message, "the message is a field, not a line on standard error");
    CY_CHECK(classified, "and it carries a classification like every other field");

    // The bridges are installed by trace_open and restored by trace_close: with no trace, base's
    // own default sink is back, and emitting through it is still safe.
    cy::emit_diagnostic(cy::DiagnosticSeverity::Info, "display", "after close");
    return cy_test::summarise("bridge");
}
