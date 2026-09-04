// Crash handler installation on POSIX hosts. See host.h; CMakeLists.txt compiles this file on
// Linux and macOS, and host_signals_windows.cpp instead on Windows.
//
// The handler runs on the crashing thread with the process in an unknown state, so everything below
// is async-signal-safe: no allocation, no locking, no stdio, no strsignal(). It calls the installed
// handler — which src/core/diagnostics/ provides at task 3.5.8 and which is bound by the same rule
// — then restores the default disposition and re-raises, so the process still dies the way the
// operating system expects and still writes whatever core file it was going to write.

#include "host.h"

#include <csignal>
#include <cstring>

namespace cy::host {
namespace {

// The signals a crash arrives as. SIGABRT is included because a failed assertion reaches the same
// place through std::abort(), and a report is as useful there as after a segmentation fault.
struct SignalEntry {
    int number;
    const char* description;
};

constexpr SignalEntry kSignals[] = {
    {SIGSEGV, "SIGSEGV: invalid memory reference"},
    {SIGBUS, "SIGBUS: bus error"},
    {SIGILL, "SIGILL: illegal instruction"},
    {SIGFPE, "SIGFPE: arithmetic exception"},
    {SIGABRT, "SIGABRT: abort"},
};

constexpr usize kSignalCount = sizeof(kSignals) / sizeof(kSignals[0]);

// Read from the signal handler, so both are the types the standard permits there. The handler
// pointer is not sig_atomic_t — no such guarantee exists for a pointer — but it is written once
// before any signal can arrive and never again while a handler is installed.
CrashHandler g_handler = nullptr;
void* g_user = nullptr;
volatile std::sig_atomic_t g_installed = 0;
struct sigaction g_previous[kSignalCount];

const char* describe(int signal_number) {
    for (const SignalEntry& entry : kSignals) {
        if (entry.number == signal_number) {
            return entry.description;
        }
    }
    return "unknown signal";
}

extern "C" void crash_signal_handler(int signal_number) {
    if (g_handler != nullptr) {
        CrashContext context;
        context.signal_or_exception = signal_number;
        context.description = describe(signal_number);
        g_handler(context, g_user);
    }

    // Restore whatever was there before and let the signal happen again, unhandled. Re-raising
    // rather than exiting is what preserves the exit status and the core dump the host would
    // otherwise have produced.
    for (usize i = 0; i < kSignalCount; ++i) {
        if (kSignals[i].number == signal_number) {
            ::sigaction(signal_number, &g_previous[i], nullptr);
            break;
        }
    }
    ::raise(signal_number);
}

}  // namespace

Status install_crash_handler(CrashHandler handler, void* user) {
    if (g_installed != 0) {
        return fail(ErrorCode::AlreadyExists, "a crash handler is already installed");
    }

    g_handler = handler;
    g_user = user;

    struct sigaction action{};
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = crash_signal_handler;
    ::sigemptyset(&action.sa_mask);
    // SA_NODEFER is deliberately absent: a fault inside the handler must not re-enter it.
    action.sa_flags = SA_RESTART;

    for (usize i = 0; i < kSignalCount; ++i) {
        if (::sigaction(kSignals[i].number, &action, &g_previous[i]) != 0) {
            // Undo the ones already installed rather than leaving the process half-handled.
            for (usize undo = 0; undo < i; ++undo) {
                ::sigaction(kSignals[undo].number, &g_previous[undo], nullptr);
            }
            g_handler = nullptr;
            g_user = nullptr;
            return fail(ErrorCode::Internal, "sigaction() refused a crash signal");
        }
    }

    g_installed = 1;
    return ok();
}

void uninstall_crash_handler() {
    if (g_installed == 0) {
        return;
    }
    for (usize i = 0; i < kSignalCount; ++i) {
        ::sigaction(kSignals[i].number, &g_previous[i], nullptr);
    }
    g_installed = 0;
    g_handler = nullptr;
    g_user = nullptr;
}

}  // namespace cy::host
