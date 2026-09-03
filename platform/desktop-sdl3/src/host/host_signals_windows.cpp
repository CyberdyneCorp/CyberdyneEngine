// Crash handler installation on Windows. See host.h; CMakeLists.txt compiles this file only on
// Windows, and host_signals_posix.cpp instead on Linux and macOS.
//
// UNVERIFIED. Written against the documented Win32 surface and reviewed; no Windows host was
// available at M0.
//
// The Windows equivalent of a fatal signal is a structured exception that no frame handled, so the
// hook is SetUnhandledExceptionFilter(). The filter runs on the faulting thread with the process in
// an unknown state, so it does exactly what its POSIX counterpart does: call the installed handler,
// then return EXCEPTION_CONTINUE_SEARCH so that Windows Error Reporting still sees the crash and
// still writes whatever dump it was going to write.

#include "host.h"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

namespace cy::host {
namespace {

CrashHandler g_handler = nullptr;
void* g_user = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
bool g_installed = false;

const char* describe(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
            return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT:
            return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
            return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:
            return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:
            return "EXCEPTION_STACK_OVERFLOW";
        default:
            return "unhandled structured exception";
    }
}

LONG WINAPI crash_exception_filter(EXCEPTION_POINTERS* exception) {
    if (g_handler != nullptr && exception != nullptr && exception->ExceptionRecord != nullptr) {
        const DWORD code = exception->ExceptionRecord->ExceptionCode;
        CrashContext context;
        context.signal_or_exception = static_cast<i32>(code);
        context.description = describe(code);
        g_handler(context, g_user);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

Status install_crash_handler(CrashHandler handler, void* user) {
    if (g_installed) {
        return fail(ErrorCode::AlreadyExists, "a crash handler is already installed");
    }
    g_handler = handler;
    g_user = user;
    g_previous = ::SetUnhandledExceptionFilter(crash_exception_filter);
    g_installed = true;
    return ok();
}

void uninstall_crash_handler() {
    if (!g_installed) {
        return;
    }
    ::SetUnhandledExceptionFilter(g_previous);
    g_previous = nullptr;
    g_handler = nullptr;
    g_user = nullptr;
    g_installed = false;
}

}  // namespace cy::host
