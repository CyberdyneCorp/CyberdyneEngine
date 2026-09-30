// The Windows half of the crash path.
//
// Run by the windows-x86_64 CI test leg: `diagnostics.crash` reads back what it writes.
//
// The constraint is the same as the POSIX side: the filter runs in a damaged process, so it
// allocates nothing, formats nothing, and writes through a raw handle. Symbol resolution is
// deliberately absent — SymFromAddr loads DbgHelp and allocates — so the report carries module
// bases and offsets, captured at installation, and symbolicates later against the archived PDBs.

#include "platform_bits.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>

#include <windows.h>

#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <psapi.h>
#include <sys/stat.h>

#pragma comment(lib, "psapi.lib")

namespace cy::diag {
namespace {

constexpr u32 kMaxFrames = 64;

// --- The module table, as the POSIX half keeps it ------------------------------------------------
//
// Captured at installation, where the loader may be asked, and read-only afterwards, so the filter
// touches no loader lock. Basenames only: the loader's path names the build machine's directories.
constexpr u32 kMaxModules = 96;
constexpr u32 kModuleNameCapacity = 64;

struct ModuleEntry {
    u64 base = 0;
    u64 end = 0;
    char name[kModuleNameCapacity] = {};
};

ModuleEntry g_modules[kMaxModules];
u32 g_module_count = 0;

void copy_basename(char* out, u32 capacity, const char* path) noexcept {
    const char* name = path;
    for (const char* cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            name = cursor + 1;
        }
    }
    u32 length = 0;
    for (; name[length] != '\0' && length + 1 < capacity; ++length) {
        out[length] = name[length];
    }
    out[length] = '\0';
}

void capture_module_table() noexcept {
    g_module_count = 0;
    HMODULE handles[kMaxModules] = {};
    DWORD needed = 0;
    const HANDLE process = ::GetCurrentProcess();
    if (::EnumProcessModules(process, handles, sizeof(handles), &needed) == 0) {
        return;
    }
    const u32 listed = static_cast<u32>(needed / sizeof(HMODULE));
    for (u32 index = 0; index < listed && index < kMaxModules; ++index) {
        MODULEINFO info{};
        if (::GetModuleInformation(process, handles[index], &info, sizeof(info)) == 0) {
            continue;
        }
        char path[MAX_PATH] = {};
        if (::GetModuleFileNameA(handles[index], path, MAX_PATH) == 0) {
            continue;
        }
        ModuleEntry& entry = g_modules[g_module_count++];
        entry.base = reinterpret_cast<u64>(info.lpBaseOfDll);
        entry.end = entry.base + info.SizeOfImage;
        copy_basename(entry.name, kModuleNameCapacity, path);
    }
}

const ModuleEntry* module_for(u64 address) noexcept {
    for (u32 index = 0; index < g_module_count; ++index) {
        if (address >= g_modules[index].base && address < g_modules[index].end) {
            return &g_modules[index];
        }
    }
    return nullptr;
}

void append_text(char* out, u32 capacity, u32& length, const char* text) noexcept {
    for (const char* cursor = text; *cursor != '\0' && length + 1 < capacity; ++cursor) {
        out[length++] = *cursor;
    }
    out[length] = '\0';
}

void append_decimal(char* out, u32 capacity, u32& length, u64 value) noexcept {
    char digits[24];
    u32 index = sizeof(digits);
    digits[--index] = '\0';
    do {
        digits[--index] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && index > 0);
    append_text(out, capacity, length, digits + index);
}

/// Sixteen nibbles, always, so every address has the one shape `crash_inspect.py` matches.
void append_hex(char* out, u32 capacity, u32& length, u64 value) noexcept {
    static const char kDigits[] = "0123456789abcdef";
    char text[19];
    text[0] = '0';
    text[1] = 'x';
    for (u32 index = 0; index < 16; ++index) {
        text[2 + index] = kDigits[(value >> (60U - (index * 4U))) & 0xFU];
    }
    text[18] = '\0';
    append_text(out, capacity, length, text);
}

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

/// std::abort() on Windows is not a structured exception: the CRT raises SIGABRT and then ends the
/// process with _exit(3), and the unhandled-exception filter above never runs. So abort is taken
/// as a signal, as on POSIX. The CRT has reset the disposition to SIG_DFL by the time this runs,
/// and returning lets abort() finish the process the way it would have.
using AbortHandler = void (*)(int);
AbortHandler g_previous_abort = SIG_DFL;

void handle_abort(int number) {
    CrashSignal signal{};
    signal.number = number;
    signal.description = "SIGABRT";
    const i32 handle = platform_create_file_new(crash_report_path());
    if (handle >= 0) {
        write_crash_report_to_fd(handle, signal);
        platform_close_file(handle);
    }
}

/// `_mkdir` succeeded, or the directory is already there. The second half is asked of the file
/// system rather than read from errno: `_mkdir("C:")` — the first prefix of every absolute path —
/// fails with EACCES or ENOENT, not EEXIST, although the drive plainly exists.
bool made_or_present(const char* directory) noexcept {
    if (::_mkdir(directory) == 0 || errno == EEXIST) {
        return true;
    }
    const DWORD attributes = ::GetFileAttributesA(directory);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
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
        if (!made_or_present(buffer)) {
            return false;
        }
        *cursor = separator;
    }
    return made_or_present(buffer);
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
    for (u32 index = 0; index < g_module_count; ++index) {
        char line[160];
        u32 length = 0;
        append_text(line, sizeof(line), length, "  ");
        append_text(line, sizeof(line), length, g_modules[index].name);
        append_text(line, sizeof(line), length, " base=");
        append_hex(line, sizeof(line), length, g_modules[index].base);
        append_text(line, sizeof(line), length, "\n");
        platform_write(handle, line, length);
    }
    return g_module_count;
}

u32 platform_write_backtrace(i32 handle) noexcept {
    void* frames[kMaxFrames];
    const USHORT count =
        ::CaptureStackBackTrace(0, static_cast<DWORD>(kMaxFrames), frames, nullptr);
    // The POSIX line: basename, offset within the module, absolute address. A frame in no captured
    // module keeps the address alone, which `crash_inspect.py`'s pattern allows for.
    for (USHORT index = 0; index < count; ++index) {
        const auto address = reinterpret_cast<u64>(frames[index]);
        const ModuleEntry* module = module_for(address);
        char line[160];
        u32 length = 0;
        append_text(line, sizeof(line), length, "  #");
        append_decimal(line, sizeof(line), length, index);
        append_text(line, sizeof(line), length, " ");
        if (module != nullptr) {
            append_text(line, sizeof(line), length, module->name);
            append_text(line, sizeof(line), length, "+");
            append_hex(line, sizeof(line), length, address - module->base);
            append_text(line, sizeof(line), length, " ");
        }
        append_text(line, sizeof(line), length, "pc=");
        append_hex(line, sizeof(line), length, address);
        append_text(line, sizeof(line), length, "\n");
        platform_write(handle, line, length);
    }
    return static_cast<u32>(count);
}

u32 platform_install_crash_handler() noexcept {
    // Before the idempotence guard, as on POSIX: reinstalling is how a module loaded since the
    // first installation gets into the table.
    capture_module_table();
    if (g_installed) {
        return 1;
    }
    g_previous = ::SetUnhandledExceptionFilter(&handle_exception);
    const AbortHandler previous_abort = ::signal(SIGABRT, &handle_abort);
    g_previous_abort = previous_abort == SIG_ERR ? SIG_DFL : previous_abort;
    g_installed = true;
    return previous_abort == SIG_ERR ? 1 : 2;
}

void platform_uninstall_crash_handler() noexcept {
    if (!g_installed) {
        return;
    }
    ::SetUnhandledExceptionFilter(g_previous);
    g_previous = nullptr;
    (void)::signal(SIGABRT, g_previous_abort);
    g_previous_abort = SIG_DFL;
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
