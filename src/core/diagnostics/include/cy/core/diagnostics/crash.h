#pragma once
// The crash artefact.
//
// `diagnostics-profiling-and-crash` — "Crash artefacts": written for the worst moment. No editor,
// no debugger, no network, possibly no symbols. The capture path assumes the process is damaged: it
// allocates nothing, takes no lock, formats nothing through the C library's variadic printers, and
// re-enters no subsystem. Everything it needs — the path, the identity strings, the report buffer —
// is computed when the handler is installed.
//
// M0 writes a text report. It carries the signal, the backtrace with module identities and offsets,
// the build identity, the last frame the process reached, and the breadcrumb ring. The trace tail,
// the module list, health state and the reproduction link are the same file's later sections;
// breadcrumbs are what the specification requires to survive when the trace does not, and they do.
//
// Where this lives. Installing an operating-system handler is platform work, and
// `core-platform-abstraction` puts it behind `Platform::install_crash_handler()`. That interface is
// task 3.2.1 and did not exist when this module was written, so the installation is two translation
// units — POSIX and Windows — selected by the build rather than by an #ifdef in a shared file. When
// the Platform interface lands, this becomes its implementation and the selection moves with it.

#include <cy/core/diagnostics/prelude.h>
#include <cy/core/diagnostics/privacy.h>

namespace cy::diag {

/// What the operating system reported. Filled by the platform handler, or by a caller reporting a
/// fault it detected itself.
struct CrashSignal {
    i32 number = 0;  // the signal number, or 0 for a synthetic report
    i32 code = 0;    // si_code, or the Windows exception code
    const void* fault_address = nullptr;
    const char* description = "";  // "SIGSEGV", "fatal assertion" — always a literal
    const char* detail = "";       // the assertion expression, if any — always a literal
};

struct CrashConfig {
    /// Where reports are written. Null takes the user mount: $CY_CRASH_DIR, else
    /// $XDG_STATE_HOME/cyberdyne/crashes, else $HOME/.local/state/cyberdyne/crashes. This is the
    /// seam that becomes `Platform::user_data_dir()` at task 3.2.1.
    const char* directory = nullptr;
    /// Recorded in the report so it identifies the build that produced it. Literals: they are read
    /// from a signal handler.
    const char* engine_version = "";
    const char* build_identity = "";
    /// What a crash report may contain. It is prepared to leave the machine, so it defaults to the
    /// upload policy: nothing classified sensitive or secret is written into it.
    ExportPolicy policy = ExportPolicy::upload();
};

/// Install the handler. Returns the number of fault conditions it took over. Idempotent: a second
/// call replaces the configuration and reports the same count.
Expected<u32, cy::Error> install_crash_handler(const CrashConfig& config) noexcept;

/// Restore the previous handlers. Called by tests; a shipping process has no reason to.
void uninstall_crash_handler() noexcept;

/// The path the next report will be written to, computed at installation so the handler formats
/// nothing. Empty when no handler is installed.
const char* crash_report_path() noexcept;

/// Write a report for a fault the caller detected — a fatal assertion, a device removal — without a
/// signal having been raised. Returns the bytes written. Uses the same path as the handler.
Expected<u64, cy::Error> write_crash_report(const CrashSignal& signal) noexcept;

/// The report body, as the signal handler writes it: an open file descriptor and nothing else.
/// Exposed so the platform translation units share one implementation of the report. Returns the
/// bytes written.
u64 write_crash_report_to_fd(i32 handle, const CrashSignal& signal) noexcept;

}  // namespace cy::diag
