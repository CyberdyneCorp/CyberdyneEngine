#include <cy/core/base/diagnostic_sink.h>

#include <cstdarg>
#include <cstdio>

namespace cy {
namespace {

// Installed once at startup and read thereafter. There is no job system at M0 (design.md §9), so
// there is no writer to race with; when one arrives this becomes an atomic.
DiagnosticSink g_sink = nullptr;
void* g_sink_user = nullptr;

const char* severity_name(DiagnosticSeverity severity) {
    switch (severity) {
        case DiagnosticSeverity::Info:
            return "info";
        case DiagnosticSeverity::Warning:
            return "warning";
        case DiagnosticSeverity::Error:
            return "error";
    }
    return "?";
}

void default_sink(DiagnosticSeverity severity, const char* category, const char* message,
                  void* /*user*/) {
    std::fprintf(stderr, "[%s] %s: %s\n", severity_name(severity), category, message);
}

}  // namespace

DiagnosticSink set_diagnostic_sink(DiagnosticSink sink, void* user) noexcept {
    DiagnosticSink previous = g_sink;
    g_sink = sink;
    g_sink_user = user;
    return previous;
}

void emit_diagnostic(DiagnosticSeverity severity, const char* category,
                     const char* message) noexcept {
    if (g_sink != nullptr) {
        g_sink(severity, category, message, g_sink_user);
    } else {
        default_sink(severity, category, message, nullptr);
    }
}

void emit_diagnosticf(DiagnosticSeverity severity, const char* category, const char* format,
                      ...) noexcept {
    char message[512];
    std::va_list arguments;
    va_start(arguments, format);
    // Truncation is the intended behaviour: a diagnostic is not worth an allocation, and a message
    // this long has already said what it needed to.
    std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    emit_diagnostic(severity, category, message);
}

}  // namespace cy
