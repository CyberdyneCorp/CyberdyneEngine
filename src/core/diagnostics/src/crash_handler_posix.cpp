// The POSIX half of the crash path: Linux, macOS and every other platform with sigaction().
//
// Selected by the build, not by an #ifdef in a shared file. Everything called from the handler is
// async-signal-safe: sigaction(), write(), open(), _exit(), and backtrace(), which walks the frame
// pointers and allocates nothing. The handler runs on its own stack, because a stack overflow is
// one of the faults it must survive long enough to report.
//
// ================================================================================================
// WHY THE BACKTRACE IS NOT WRITTEN BY backtrace_symbols_fd()
// ================================================================================================
//
// `diagnostics-profiling-and-crash` — "WHEN any produced trace, log or crash artefact is inspected
// for strings THEN it SHALL contain no absolute path from the build machine, and the check SHALL be
// a gate rather than a review". The artefact's own header declares it `potentially-personal` and it
// is prepared to leave the machine, so this is a leak rather than an untidiness.
//
// `backtrace_symbols_fd()` writes each frame as `<module>(<symbol>+<offset>)[<address>]`, and
// `<module>` is the path the loader resolved — which for an installed binary, a continuous
// integration runner or a double-clicked game is an absolute path carrying the account name. M9's
// `m9:crash-artefact-paths` criterion is the gate that found it, and it reported green for one
// evaluation only because the probe happened to be invoked by a RELATIVE path.
//
// So the frames are written here instead: a BASENAME, an offset within the module, and the absolute
// address. The name and the load base come from a module table captured when the handler is
// INSTALLED, because `dl_iterate_phdr()` takes the loader's lock and is not async-signal-safe —
// the fault path only reads a fixed array, exactly as it does for breadcrumbs and health.
//
// What is lost is the symbol name glibc could sometimes resolve from the dynamic table; what is
// gained is that `tools/trace/crash_inspect.py --symbolicate` reads the same offsets against the
// symbols the build archived, on a machine that has them, which is what the specification says
// symbolication is for.

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

// `link.h` is where `dl_iterate_phdr()` and `dl_phdr_info` live on glibc, musl, the BSDs and
// (since 10.15) Apple's dyld. Absent, the module table is empty and every frame is written as an
// absolute address with no name — narrower, and still carrying no path, which is the property this
// file exists to keep.
#if defined(__has_include)
#    if __has_include(<link.h>)
#        define CY_DIAG_HAVE_DL_ITERATE_PHDR 1
#    endif
#endif
#ifdef CY_DIAG_HAVE_DL_ITERATE_PHDR
#    include <link.h>
#endif
#if defined(__APPLE__)
#    include <mach-o/dyld.h>
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

// --- The module table ---------------------------------------------------------------------------
//
// Fixed storage, filled once at installation and read-only afterwards, so the fault path touches no
// loader lock and no allocator. Ninety-six entries covers a game's shared-object set with room to
// spare; a process with more loses the tail, and the frames in it are written with no name rather
// than with a wrong one.

constexpr u32 kMaxModules = 96;
/// Long enough for the longest basename anything links against here; a longer one is truncated,
/// which loses characters rather than leaking a directory.
constexpr u32 kModuleNameCapacity = 64;

struct ModuleEntry {
    u64 base = 0;  ///< the load bias: an address minus this is the offset addr2line wants
    u64 low = 0;   ///< the mapped span, so a frame is attributed to exactly one module
    u64 high = 0;
    char name[kModuleNameCapacity] = {};  ///< BASENAME ONLY — see the header comment
};

ModuleEntry g_modules[kMaxModules];
u32 g_module_count = 0;

/// Copy the part of `path` after the last separator. The whole point of this file: a directory is
/// never copied, so there is nothing to redact later.
void copy_basename(char* out, u32 capacity, const char* path) noexcept {
    const char* name = path;
    for (const char* cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') {
            name = cursor + 1;
        }
    }
    u32 length = 0;
    while (name[length] != '\0' && length + 1 < capacity) {
        out[length] = name[length];
        ++length;
    }
    out[length] = '\0';
}

/// The main executable's own name. `dl_iterate_phdr()` reports it with an EMPTY `dlpi_name`, and a
/// frame in the program itself is the one a reader cares about most, so it is resolved here — at
/// installation, where reading a file is allowed — rather than left as `<unknown>`.
void main_module_name(char* out, u32 capacity) noexcept {
    out[0] = '\0';
#if defined(__linux__)
    char resolved[512];
    const ssize_t length = ::readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);
    if (length > 0) {
        resolved[length] = '\0';
        copy_basename(out, capacity, resolved);
        return;
    }
#elif defined(__APPLE__)
    char resolved[1024];
    uint32_t size = sizeof(resolved);
    if (::_NSGetExecutablePath(resolved, &size) == 0) {
        copy_basename(out, capacity, resolved);
        return;
    }
#endif
    copy_basename(out, capacity, "main-executable");
}

#ifdef CY_DIAG_HAVE_DL_ITERATE_PHDR
/// One loaded object. Returns non-zero to stop the walk, which is how the table's bound is enforced
/// without a partial entry being left behind.
int collect_module(struct dl_phdr_info* info, size_t /*size*/, void* /*user*/) noexcept {
    if (g_module_count >= kMaxModules) {
        return 1;
    }
    u64 low = 0;
    u64 high = 0;
    bool mapped = false;
    for (u16 index = 0; index < info->dlpi_phnum; ++index) {
        // `auto` rather than `ElfW(Phdr)`: the macro expands to a type whose reference spelling
        // the formatter cannot parse, and the type is right there on the line it comes from.
        const auto& header = info->dlpi_phdr[index];
        if (header.p_type != PT_LOAD) {
            continue;
        }
        const u64 start = static_cast<u64>(info->dlpi_addr) + static_cast<u64>(header.p_vaddr);
        const u64 end = start + static_cast<u64>(header.p_memsz);
        if (!mapped || start < low) {
            low = start;
        }
        if (!mapped || end > high) {
            high = end;
        }
        mapped = true;
    }
    if (!mapped || low >= high) {
        return 0;  // a linker map entry with nothing loaded behind it attributes no frame
    }

    ModuleEntry& entry = g_modules[g_module_count];
    entry.base = static_cast<u64>(info->dlpi_addr);
    entry.low = low;
    entry.high = high;
    if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') {
        copy_basename(entry.name, kModuleNameCapacity, info->dlpi_name);
    } else {
        main_module_name(entry.name, kModuleNameCapacity);
    }
    ++g_module_count;
    return 0;
}
#endif

/// Capture the table. Called from `platform_install_crash_handler()` — before the guard that makes
/// a second installation a no-op, so a process that reinstalls after dlopen()ing something sees it.
void capture_module_table() noexcept {
    g_module_count = 0;
#ifdef CY_DIAG_HAVE_DL_ITERATE_PHDR
    ::dl_iterate_phdr(&collect_module, nullptr);
#endif
}

/// The module an address falls in, or null. A linear walk over at most ninety-six entries, which is
/// what "allocates nothing and takes no lock" costs and is unmeasurable against writing the file.
const ModuleEntry* module_for(u64 address) noexcept {
    for (u32 index = 0; index < g_module_count; ++index) {
        const ModuleEntry& entry = g_modules[index];
        if (address >= entry.low && address < entry.high) {
            return &entry;
        }
    }
    return nullptr;
}

// --- Formatting, by hand ------------------------------------------------------------------------
//
// The same reason crash_report.cpp formats by hand: the fault path calls no variadic printer. These
// are local rather than shared because they compose into a line buffer, not into a descriptor, so
// one frame is one write() and a report interleaved with another writer's output stays readable.

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

/// Sixteen nibbles, always, so that a column of addresses lines up and `crash_inspect.py`'s pattern
/// has one shape to match.
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

u32 platform_write_module_table(i32 handle) noexcept {
    for (u32 index = 0; index < g_module_count; ++index) {
        const ModuleEntry& entry = g_modules[index];
        char line[160];
        u32 length = 0;
        append_text(line, sizeof(line), length, "  ");
        append_text(line, sizeof(line), length, entry.name);
        append_text(line, sizeof(line), length, " base=");
        append_hex(line, sizeof(line), length, entry.base);
        append_text(line, sizeof(line), length, "\n");
        platform_write(handle, line, length);
    }
    return g_module_count;
}

u32 platform_write_backtrace(i32 handle) noexcept {
#ifdef CY_DIAG_HAVE_EXECINFO
    void* frames[kMaxFrames];
    const int count = ::backtrace(frames, static_cast<int>(kMaxFrames));
    if (count <= 0) {
        return 0;
    }
    // One line per frame, written here rather than by backtrace_symbols_fd(): basename, offset
    // within the module, absolute address. `crash_inspect.py` reads the offset; the address is kept
    // because it is what a core file and a debugger agree with, and it names no file.
    for (int index = 0; index < count; ++index) {
        const auto address = reinterpret_cast<u64>(frames[index]);
        const ModuleEntry* module = module_for(address);
        char line[160];
        u32 length = 0;
        append_text(line, sizeof(line), length, "  #");
        append_decimal(line, sizeof(line), length, static_cast<u64>(index));
        append_text(line, sizeof(line), length, " ");
        append_text(line, sizeof(line), length, (module != nullptr) ? module->name : "<unknown>");
        append_text(line, sizeof(line), length, "+");
        append_hex(line, sizeof(line), length, (module != nullptr) ? (address - module->base) : 0U);
        append_text(line, sizeof(line), length, " pc=");
        append_hex(line, sizeof(line), length, address);
        append_text(line, sizeof(line), length, "\n");
        platform_write(handle, line, length);
    }
    return static_cast<u32>(count);
#else
    (void)handle;
    return 0;
#endif
}

u32 platform_install_crash_handler() noexcept {
    // Before the idempotence guard, deliberately: reinstalling is how a process that has dlopen()ed
    // a module since the first install gets it into the table, and the walk is the only part of
    // this function that is not already a no-op the second time.
    capture_module_table();
    if (g_installed) {
        return kFaultCount;
    }
    stack_t alternate{};
    alternate.ss_sp = g_alternate_stack;
    alternate.ss_size = sizeof(g_alternate_stack);
    ::sigaltstack(&alternate, nullptr);

    struct sigaction action{};
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
