// The Windows half of the crash path.
//
// UNVERIFIED. It was written against the documented behaviour of SetUnhandledExceptionFilter,
// CaptureStackBackTrace and the CRT's low-level I/O, and it has never been compiled or run: the
// machine this milestone was implemented on is Linux, and M0's CI does not exist yet (task 2.4.x).
// It is here because leaving the platform out entirely would hide the shape of the port; treat the
// first Windows build as a review of this file.
//
// The constraint is the same as the POSIX side: the filter runs in a damaged process, so it
// allocates nothing, formats nothing, and writes through a raw handle. Symbol resolution is
// deliberately absent — SymFromAddr loads DbgHelp and allocates — so the report carries module
// bases and offsets, and symbolicates later against the archived PDBs.

#include "platform_bits.h"

#include <cstdlib>
#include <cstring>

#include <windows.h>

#include <direct.h>
#include <io.h>
#include <process.h>

namespace cy::diag {
namespace {

constexpr u32 kMaxFrames = 64;

LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
bool g_installed = false;

LONG WINAPI handle_exception(EXCEPTION_POINTERS* pointers) {
    CrashSignal signal{};
    if (pointers != nullptr && pointers->ExceptionRecord != nullptr) {
        signal.number = static_cast<i32>(pointers->ExceptionRecord->ExceptionCode);
        signal.code = static_cast<i32>(pointers->ExceptionRecord->NumberParameters);
        signal.fault_address = pointers->ExceptionRecord->ExceptionAddress;
    }
    signal.description = platform_fault_name(signal.number);

    const i32 handle = platform_create_file_new(crash_report_path());
    if (handle >= 0) {
        write_crash_report_to_fd(handle, signal);
        platform_close_file(handle);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

u32 platform_process_id() noexcept {
    return static_cast<u32>(::GetCurrentProcessId());
}

bool platform_make_directories(const char* path) noexcept {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    char buffer[512];
    std::strncpy(buffer, path, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    for (char* cursor = buffer + 1; *cursor != '\0'; ++cursor) {
        if (*cursor != '\\' && *cursor != '/') {
            continue;
        }
        const char separator = *cursor;
        *cursor = '\0';
        if (::_mkdir(buffer) != 0 && errno != EEXIST) {
            return false;
        }
        *cursor = separator;
    }
    return ::_mkdir(buffer) == 0 || errno == EEXIST;
}

void platform_default_crash_directory(char* buffer, u32 capacity) noexcept {
    const char* override_path = std::getenv("CY_CRASH_DIR");
    const char* local_app_data = std::getenv("LOCALAPPDATA");
    buffer[0] = '\0';
    if (override_path != nullptr && override_path[0] != '\0') {
        std::strncpy(buffer, override_path, capacity - 1);
    } else if (local_app_data != nullptr && local_app_data[0] != '\0') {
        std::strncpy(buffer, local_app_data, capacity - 1);
        std::strncat(buffer, "\\Cyberdyne\\crashes", capacity - std::strlen(buffer) - 1);
    } else {
        std::strncpy(buffer, ".", capacity - 1);
    }
}

i32 platform_create_file(const char* path) noexcept {
    int handle = -1;
    ::_sopen_s(&handle, path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _SH_DENYNO,
               _S_IREAD | _S_IWRITE);
    return static_cast<i32>(handle);
}

i32 platform_create_file_new(const char* path) noexcept {
    int handle = -1;
    ::_sopen_s(&handle, path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _SH_DENYNO,
               _S_IREAD | _S_IWRITE);
    return static_cast<i32>(handle);
}

void platform_close_file(i32 handle) noexcept {
    if (handle >= 0) {
        ::_close(handle);
    }
}

i64 platform_write(i32 handle, const void* data, usize bytes) noexcept {
    if (handle < 0) {
        return -1;
    }
    return static_cast<i64>(::_write(handle, data, static_cast<unsigned int>(bytes)));
}

u32 platform_write_module_table(i32 handle) noexcept {
    // NOT IMPLEMENTED HERE, AND SAID RATHER THAN FAKED. The POSIX half captures the table with
    // dl_iterate_phdr() at installation so the artefact can carry a basename instead of the
    // loader's absolute path; the Windows equivalent is EnumProcessModules() plus
    // GetModuleFileNameA() in the same place, and this file has never been compiled — see the
    // header comment. Writing nothing keeps the property that matters (`crash_report.cpp` prints
    // "<no module table on this platform>" and the frames below carry no path either); what is
    // missing is the name beside each offset, not the redaction.
    (void)handle;
    return 0;
}

u32 platform_write_backtrace(i32 handle) noexcept {
    void* frames[kMaxFrames];
    const USHORT count =
        ::CaptureStackBackTrace(0, static_cast<DWORD>(kMaxFrames), frames, nullptr);
    for (USHORT index = 0; index < count; ++index) {
        // `  #<n> pc=0x…`: the POSIX line without the module half, because there is no table to
        // resolve it against. `crash_inspect.py`'s frame pattern makes that half optional for
        // exactly this case rather than requiring a `<unknown>+0x0` that claims an offset.
        char line[40];
        u32 length = 0;
        line[length++] = ' ';
        line[length++] = ' ';
        line[length++] = '#';
        if (index >= 10) {
            line[length++] = static_cast<char>('0' + (index / 10));
        }
        line[length++] = static_cast<char>('0' + (index % 10));
        line[length++] = ' ';
        line[length++] = 'p';
        line[length++] = 'c';
        line[length++] = '=';
        line[length++] = '0';
        line[length++] = 'x';
        const u64 address = reinterpret_cast<u64>(frames[index]);
        static const char digits[] = "0123456789abcdef";
        for (u32 nibble = 0; nibble < 16; ++nibble) {
            line[length++] = digits[(address >> (60u - (nibble * 4u))) & 0xFu];
        }
        line[length++] = '\n';
        platform_write(handle, line, length);
    }
    return static_cast<u32>(count);
}

u32 platform_install_crash_handler() noexcept {
    if (g_installed) {
        return 1;
    }
    g_previous = ::SetUnhandledExceptionFilter(&handle_exception);
    g_installed = true;
    return 1;
}

void platform_uninstall_crash_handler() noexcept {
    if (!g_installed) {
        return;
    }
    ::SetUnhandledExceptionFilter(g_previous);
    g_previous = nullptr;
    g_installed = false;
}

const char* platform_fault_name(i32 number) noexcept {
    switch (static_cast<DWORD>(number)) {
        case EXCEPTION_ACCESS_VIOLATION:
            return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:
            return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
            return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case 0:
            return "none";
        default:
            return "exception";
    }
}

}  // namespace cy::diag
