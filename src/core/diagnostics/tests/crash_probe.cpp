// A program that really crashes, so the crash artefact is proved by reading one back.
//
// Run by test_crash.py, which starts it, lets it die, and reads the report it left behind. It is a
// separate executable because the only honest test of a signal handler is a signal.

#include <cy/core/base/assert.h>

#include <cy/core/diagnostics/breadcrumb.h>
#include <cy/core/diagnostics/bridge.h>
#include <cy/core/diagnostics/crash.h>
#include <cy/core/diagnostics/log.h>
#include <cy/core/diagnostics/trace.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// UndefinedBehaviorSanitizer diagnoses the store below before the hardware faults, so the process
// exits through the tool rather than through a signal and there is no crash artefact to read back.
// That is the sanitizer working, not the handler failing, so this mode reports itself skipped in an
// instrumented binary — the same shape the assertion mode already uses when CY_ASSERT is compiled
// out. The other three modes still run, and an uninstrumented build still proves the signal path.
// GCC defines no macro for it, so cmake/sanitizers.cmake defines CY_SANITIZE_UNDEFINED when the
// build asked for it. Clang's __has_feature is checked too, so a hand-rolled clang build with
// -fsanitize=undefined and no CMake still gets the right answer.
#if defined(CY_SANITIZE_UNDEFINED)
#    define CY_PROBE_UBSAN 1
#endif
#if !defined(CY_PROBE_UBSAN) && defined(__has_feature)
#    if __has_feature(undefined_behavior_sanitizer)
#        define CY_PROBE_UBSAN 1
#    endif
#endif

using namespace cy::diag;

namespace {
CY_TRACE_CATEGORY(probe_category, "probe")
CY_TRACE_NAME(probe_event, "probe.working")
}  // namespace

int main(int argc, char** argv) {
    const char* directory = (argc > 1) ? argv[1] : ".";
    const char* mode = (argc > 2) ? argv[2] : "segv";

    CrashConfig crash;
    crash.directory = directory;
    crash.engine_version = "0.0.0-probe";
    crash.build_identity = "crash-probe";
    const auto installed = install_crash_handler(crash);
    if (!installed) {
        std::fprintf(stderr, "install failed: %s\n", error_code_name(installed.error().code));
        return 2;
    }
    // The test finds the report through this line; the path was composed at installation, not now.
    std::printf("%s\n", crash_report_path());
    std::fflush(stdout);

    TraceConfig config;
    config.path = "cy_diag_crash_probe.cytrace";
    config.consumer_thread = false;
    (void)trace_open(config);

    trace_frame_begin(4242);
    CY_BREADCRUMB("level.transition", 7);
    CY_BREADCRUMB("asset.activation", 99);
    trace_instant(probe_event(), probe_category(), Channel::Important);

    if (std::strcmp(mode, "assert") == 0) {
#if defined(CY_DEVELOPMENT)
        // A real CY_ASSERT failure, routed through the bridge trace_open() installed: it records,
        // writes the artefact, and base aborts as it always would have.
        const int resident = 4096;
        CY_ASSERT_MSG(resident < 0, "resident bytes must be negative, which they never are");
        return 0;
#else
        // Profile and Shipping compile assertions out (design.md §7), so there is no failed
        // assertion to produce an artefact from. Say so on the line the caller reads as a path,
        // rather than leaving it to conclude the crash handler is broken.
        std::printf("assertions-compiled-out\n");
        return 0;
#endif
    }
    if (std::strcmp(mode, "report") == 0) {
        CrashSignal signal;
        signal.description = "synthetic";
        const auto written = write_crash_report(signal);
        return written.has_value() ? 0 : 3;
    }
    if (std::strcmp(mode, "abort") == 0) {
        std::abort();
    }
#if defined(CY_PROBE_UBSAN)
    std::printf("mode-not-available\n");
    return 0;
#else
    // A real fault: a store through an address the compiler cannot fold to a literal null, so the
    // fault happens at run time rather than being diagnosed at compile time.
    const char* address = (argc > 3) ? argv[3] : "0";
    // The address is the point: this probe exists to fault at run time, so the compiler must not
    // be able to fold it to a literal null.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* target = reinterpret_cast<volatile int*>(std::strtoull(address, nullptr, 0));
    *target = 1;
    return 0;
#endif
}
