// The POSIX half of the crash path: Linux, macOS and every other platform with sigaction().
//
// Selected by the build, not by an #ifdef in a shared file. Everything called from the handler is
// async-signal-safe: sigaction(), write(), open(), _exit(), and glibc's backtrace_symbols_fd(),
// which is documented as not calling malloc. The handler runs on its own stack, because a stack
// overflow is one of the faults it must survive long enough to report.

#include "platform_bits.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__has_include)
#    if __has_include(<execinfo.h>)
#        define CY_DIAG_HAVE_EXECINFO 1
#    endif
#endif
#ifdef CY_DIAG_HAVE_EXECINFO
#    include <execinfo.h>
#endif

namespace cy::diag {
namespace {

/// The faults worth taking over. SIGABRT is included because a fatal assertion arrives that way,
/// and a report is more useful than a core file nobody has the symbols for.
constexpr i32 kFaults[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
constexpr u32 kFaultCount = sizeof(kFaults) / sizeof(kFaults[0]);
constexpr u32 kMaxFrames = 64;

struct sigaction g_previous[kFaultCount];
bool g_installed = false;
// SIGSTKSZ is not a constant on glibc 2.34 and later, so the size is stated here.
constexpr u32 kAlternateStackBytes = 65536;
char g_alternate_stack[kAlternateStackBytes];

void handle_fault(int number, siginfo_t* info, void* /*context*/) {
    CrashSignal signal{};
    signal.number = number;
    signal.code = (info != nullptr) ? info->si_code : 0;
    signal.fault_address = (info != nullptr) ? info->si_addr : nullptr;
    signal.description = platform_fault_name(number);

    const i32 handle = platform_create_file_new(crash_report_path());
    if (handle >= 0) {
        write_crash_report_to_fd(handle, signal);
        platform_close_file(handle);
    }

    // Restore the previous disposition and re-raise, so the process dies the way it would have and
    // a debugger or a core dump still sees the original fault.
    platform_uninstall_crash_handler();
    ::raise(number);
}

}  // namespace

u32 platform_process_id() noexcept {
    return static_cast<u32>(::getpid());
}

bool platform_make_directories(const char* path) noexcept {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    char buffer[512];
    std::strncpy(buffer, path, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    for (char* cursor = buffer + 1; *cursor != '\0'; ++cursor) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        if (::mkdir(buffer, 0755) != 0 && errno != EEXIST) {
            return false;
        }
        *cursor = '/';
    }
    return ::mkdir(buffer, 0755) == 0 || errno == EEXIST;
}

void platform_default_crash_directory(char* buffer, u32 capacity) noexcept {
    const char* override_path = std::getenv("CY_CRASH_DIR");
    if (override_path != nullptr && override_path[0] != '\0') {
        std::strncpy(buffer, override_path, capacity - 1);
        return;
    }
    const char* state_home = std::getenv("XDG_STATE_HOME");
    const char* home = std::getenv("HOME");
    buffer[0] = '\0';
    if (state_home != nullptr && state_home[0] != '\0') {
        std::strncpy(buffer, state_home, capacity - 1);
        std::strncat(buffer, "/cyberdyne/crashes", capacity - std::strlen(buffer) - 1);
    } else if (home != nullptr && home[0] != '\0') {
        std::strncpy(buffer, home, capacity - 1);
        std::strncat(buffer, "/.local/state/cyberdyne/crashes", capacity - std::strlen(buffer) - 1);
    } else {
        std::strncpy(buffer, ".", capacity - 1);
    }
}

i32 platform_create_file(const char* path) noexcept {
    return static_cast<i32>(::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644));
}

i32 platform_create_file_new(const char* path) noexcept {
    return static_cast<i32>(::open(path, O_WRONLY | O_CREAT | O_EXCL, 0644));
}

void platform_close_file(i32 handle) noexcept {
    if (handle >= 0) {
        ::close(handle);
    }
}

i64 platform_write(i32 handle, const void* data, usize bytes) noexcept {
    if (handle < 0) {
        return -1;
    }
    return static_cast<i64>(::write(handle, data, bytes));
}

u32 platform_write_backtrace(i32 handle) noexcept {
#ifdef CY_DIAG_HAVE_EXECINFO
    void* frames[kMaxFrames];
    const int count = ::backtrace(frames, static_cast<int>(kMaxFrames));
    if (count <= 0) {
        return 0;
    }
    // Writes module, symbol and offset per frame without allocating; the report is readable on a
    // machine with no symbols, and symbolicates later against the archived ones.
    ::backtrace_symbols_fd(frames, count, handle);
    return static_cast<u32>(count);
#else
    (void)handle;
    return 0;
#endif
}

u32 platform_install_crash_handler() noexcept {
    if (g_installed) {
        return kFaultCount;
    }
    stack_t alternate{};
    alternate.ss_sp = g_alternate_stack;
    alternate.ss_size = sizeof(g_alternate_stack);
    ::sigaltstack(&alternate, nullptr);

    struct sigaction action {};
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    action.sa_sigaction = &handle_fault;
    ::sigemptyset(&action.sa_mask);

    u32 taken = 0;
    for (u32 index = 0; index < kFaultCount; ++index) {
        if (::sigaction(kFaults[index], &action, &g_previous[index]) == 0) {
            ++taken;
        }
    }
    g_installed = true;
    return taken;
}

void platform_uninstall_crash_handler() noexcept {
    if (!g_installed) {
        return;
    }
    for (u32 index = 0; index < kFaultCount; ++index) {
        ::sigaction(kFaults[index], &g_previous[index], nullptr);
    }
    g_installed = false;
}

const char* platform_fault_name(i32 number) noexcept {
    switch (number) {
        case SIGSEGV:
            return "SIGSEGV";
        case SIGBUS:
            return "SIGBUS";
        case SIGILL:
            return "SIGILL";
        case SIGFPE:
            return "SIGFPE";
        case SIGABRT:
            return "SIGABRT";
        case 0:
            return "none";
        default:
            return "signal";
    }
}

}  // namespace cy::diag
