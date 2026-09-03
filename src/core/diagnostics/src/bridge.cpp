#include <cy/core/diagnostics/bridge.h>

#include <cy/core/diagnostics/breadcrumb.h>
#include <cy/core/diagnostics/crash.h>
#include <cy/core/diagnostics/log.h>
#include <cy/core/diagnostics/trace.h>

#include <cy/core/base/assert.h>
#include <cy/core/base/diagnostic_sink.h>

#include <atomic>
#include <cstring>

namespace cy::diag {
namespace {

CY_LOG_CATEGORY(assert_category, "assert")
CY_TRACE_NAME(assertion_failed, "assertion.failed")
CY_TRACE_FIELD(assert_expression, string, cy::Privacy::Developer)
CY_TRACE_FIELD(assert_message, string, cy::Privacy::Developer)
CY_TRACE_FIELD(assert_line, u64, cy::Privacy::Public)

CY_TRACE_FIELD(diagnostic_message, string, cy::Privacy::Developer)

std::atomic<bool> g_assertion_installed{false};
std::atomic<bool> g_diagnostic_installed{false};

u32 text_length(const char* text) noexcept {
    if (text == nullptr) {
        return 0;
    }
    const usize length = std::strlen(text);
    return static_cast<u32>((length > kMaxTextBytesPerRecord) ? kMaxTextBytesPerRecord : length);
}

/// Called by base after CY_ASSERT failed and before it aborts. Everything here is on the way to a
/// process that is about to end, so it records and returns rather than deciding anything.
void on_assertion_failure(const ::cy::AssertionFailure& failure, void* /*user*/) {
    CY_BREADCRUMB("assertion.failed", failure.line);

    const FieldValue fields[] = {
        field_text(assert_expression(), failure.expression, text_length(failure.expression)),
        field_text(assert_message(), failure.message, text_length(failure.message)),
        field_u64(assert_line(), failure.line),
    };
    log_emit(assert_category(), LogLevel::Fatal, assertion_failed(), register_name(failure.file),
             fields, 3);
    trace_flush();

    // A failed assertion is a fault the engine detected itself, and it deserves the same artefact a
    // signal produces. Written only when a crash handler is installed, because that is what
    // prepared the path this must not compose now.
    if (crash_report_path()[0] != '\0') {
        CrashSignal signal;
        signal.description = "fatal assertion";
        signal.detail = failure.expression;
        (void)write_crash_report(signal);
    }
}

LogLevel level_of(::cy::DiagnosticSeverity severity) noexcept {
    switch (severity) {
        case ::cy::DiagnosticSeverity::Error:
            return LogLevel::Error;
        case ::cy::DiagnosticSeverity::Warning:
            return LogLevel::Warning;
        case ::cy::DiagnosticSeverity::Info:
        default:
            return LogLevel::Info;
    }
}

/// base's sink is a category and a formatted message, because it exists for callers that have no
/// structured vocabulary of their own. The message becomes one classified text field, so it is
/// redactable like anything else rather than being an unclassified string in the artefact.
void on_diagnostic(::cy::DiagnosticSeverity severity, const char* category, const char* message,
                   void* /*user*/) {
    const CategoryId id = register_category((category != nullptr) ? category : "diagnostic");
    const LogLevel level = level_of(severity);
    if (!log_should_emit(id, level)) {
        return;
    }
    const FieldValue fields[] = {field_text(diagnostic_message(), message, text_length(message))};
    log_emit(id, level, register_name("diagnostic"), kInvalidName, fields, 1);
}

}  // namespace

bool install_assertion_bridge() noexcept {
    if (g_assertion_installed.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }
    ::cy::set_assertion_handler(&on_assertion_failure, nullptr);
    return true;
}

void uninstall_assertion_bridge() noexcept {
    if (g_assertion_installed.exchange(false, std::memory_order_acq_rel)) {
        ::cy::set_assertion_handler(nullptr, nullptr);
    }
}

bool install_diagnostic_bridge() noexcept {
    if (g_diagnostic_installed.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }
    ::cy::set_diagnostic_sink(&on_diagnostic, nullptr);
    return true;
}

void uninstall_diagnostic_bridge() noexcept {
    if (g_diagnostic_installed.exchange(false, std::memory_order_acq_rel)) {
        ::cy::set_diagnostic_sink(nullptr, nullptr);
    }
}

}  // namespace cy::diag
