// The seam between layer 0 and the diagnostics module. Part of task 3.1.2.
//
// `core-platform-abstraction` requires that an unsupported window feature "SHALL be ignored with a
// warning, not fail window creation". Something therefore has to emit a warning from platform/,
// which is layer 3, into a logger that lives in src/core/diagnostics/ — a module written under
// tasks 3.5.x by a different author, and one this file must not reach into.
//
// So this is the smallest interface that discharges the requirement: a function pointer, a
// severity, a category and a message. src/core/diagnostics/ installs its structured logger here at
// startup and every warning already emitted through it lands in the trace timeline. Until it does,
// the default sink writes one line to stderr, so a warning is never silently dropped.
//
// This is a hook, not a logging API. Categories, levels, structured fields and privacy
// classification belong to `diagnostics-profiling-and-crash`; nothing here should grow toward them.

#pragma once

// The format-checking attribute, where the compiler has one. MSVC has no equivalent and ignores an
// unknown [[gnu::...]] with a warning that -WX would turn into an error, so it is spelled the way
// GCC and Clang spell it and left empty elsewhere.
#if defined(__GNUC__) || defined(__clang__)
#    define CY_PRINTF_FORMAT(format_index, first_argument) \
        __attribute__((format(printf, format_index, first_argument)))
#else
#    define CY_PRINTF_FORMAT(format_index, first_argument)
#endif

namespace cy {

enum class DiagnosticSeverity {
    Info,
    Warning,
    Error,
};

// `category` is a short stable identifier — "display", "platform" — not a sentence.
using DiagnosticSink = void (*)(DiagnosticSeverity severity, const char* category,
                                const char* message, void* user);

// Installs the sink and returns the previous one. nullptr restores the default.
DiagnosticSink set_diagnostic_sink(DiagnosticSink sink, void* user) noexcept;

void emit_diagnostic(DiagnosticSeverity severity, const char* category,
                     const char* message) noexcept;

// The printf-shaped form, for the common case where the message names the thing that was refused.
// Formats into a bounded stack buffer and truncates rather than allocating: there are no allocators
// at M0 (design.md §9).
void emit_diagnosticf(DiagnosticSeverity severity, const char* category, const char* format,
                      ...) noexcept CY_PRINTF_FORMAT(3, 4);

}  // namespace cy
