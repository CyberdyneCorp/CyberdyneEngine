// The crash artefact's body, written into a file descriptor and nothing else.
//
// `diagnostics-profiling-and-crash` — "Crash artefacts": self-contained, symbol-independent, and
// written with no editor, no network and no debugger present. The capture path assumes the process
// is damaged, so this file allocates nothing, takes no lock, calls no variadic formatter, and
// re-enters no subsystem. Integers are formatted by hand into stack buffers; strings are literals
// the caller supplied when it installed the handler.
//
// What the report carries at M0: build identity, the fault, the process and thread it happened on,
// the last frame the process reached, the breadcrumb ring, and the backtrace with module identities
// and offsets. Symbolication happens later, against the symbols the build archived — the report
// itself is usable on a machine that has none.

#include <cy/core/diagnostics/crash.h>

#include "internal.h"
#include "platform_bits.h"

#include <cy/core/diagnostics/breadcrumb.h>
#include <cy/core/diagnostics/field.h>
#include <cy/core/diagnostics/trace.h>

#include <atomic>
#include <cstring>

namespace cy::diag {
namespace {

constexpr u32 kPathCapacity = 512;

/// The configuration, flattened at installation into storage the handler can read without touching
/// anything that allocates.
struct CrashState {
    char path[kPathCapacity] = {};
    const char* engine_version = "";
    const char* build_identity = "";
    ExportPolicy policy = ExportPolicy::upload();
    std::atomic<bool> installed{false};
};

CrashState& state() noexcept {
    static CrashState instance;
    return instance;
}

/// A descriptor and a byte count. Every write goes through one call; nothing is buffered, because a
/// buffer that is not flushed when the process dies is a report that does not exist.
class SafeStream {
public:
    explicit SafeStream(i32 handle) noexcept : handle_(handle) {}

    void text(const char* value) noexcept {
        if (value == nullptr) {
            return;
        }
        const usize length = std::strlen(value);
        const i64 written = platform_write(handle_, value, length);
        if (written > 0) {
            bytes_ += static_cast<u64>(written);
        }
    }

    void number(u64 value) noexcept {
        char digits[24];
        u32 index = sizeof(digits);
        digits[--index] = '\0';
        do {
            digits[--index] = static_cast<char>('0' + (value % 10));
            value /= 10;
        } while (value != 0 && index > 0);
        text(digits + index);
    }

    void signed_number(i64 value) noexcept {
        if (value < 0) {
            text("-");
            number(static_cast<u64>(-value));
        } else {
            number(static_cast<u64>(value));
        }
    }

    void hex(u64 value) noexcept {
        static const char kDigits[] = "0123456789abcdef";
        char out[19];
        out[0] = '0';
        out[1] = 'x';
        for (u32 index = 0; index < 16; ++index) {
            out[2 + index] = kDigits[(value >> (60u - (index * 4u))) & 0xFu];
        }
        out[18] = '\0';
        text(out);
    }

    void line(const char* key, const char* value) noexcept {
        text(key);
        text(": ");
        text((value != nullptr) ? value : "");
        text("\n");
    }

    [[nodiscard]] u64 bytes() const noexcept { return bytes_; }
    [[nodiscard]] i32 handle() const noexcept { return handle_; }

private:
    i32 handle_;
    u64 bytes_ = 0;
};

void write_breadcrumbs(SafeStream& out) noexcept {
    Breadcrumb crumbs[kBreadcrumbCapacity];
    const u32 count = breadcrumb_snapshot(crumbs, kBreadcrumbCapacity);
    out.text("\n[breadcrumbs] ");
    out.number(count);
    out.text(" of ");
    out.number(kBreadcrumbCapacity);
    out.text("\n");
    for (u32 index = 0; index < count; ++index) {
        const Breadcrumb& crumb = crumbs[index];
        out.text("  #");
        out.number(crumb.sequence);
        out.text(" ");
        const char* phase = lookup_name(crumb.phase);
        out.text((phase != nullptr) ? phase : "<unregistered>");
        out.text(" detail=");
        out.number(crumb.detail);
        out.text(" t=");
        out.number(crumb.timestamp_ns);
        out.text("\n");
    }
}

void write_identity(SafeStream& out, const CrashState& crash) noexcept {
    out.text("cyberdyne-crash-report 1\n");
    out.line("engine_version", crash.engine_version);
    out.line("build_configuration", CY_DIAG_BUILD_CONFIGURATION);
    out.line("build_identity", crash.build_identity);
    // What the artefact may contain, declared, because it is prepared to leave the machine.
    out.line("classifications", privacy_name(crash.policy.ceiling()));
    out.text("process: ");
    out.number(platform_process_id());
    out.text("\n");
}

void write_fault(SafeStream& out, const CrashSignal& signal) noexcept {
    out.text("\n[fault]\n");
    out.line("description", signal.description);
    out.text("signal: ");
    out.signed_number(signal.number);
    out.text(" (");
    out.text(platform_fault_name(signal.number));
    out.text(")\n");
    out.text("code: ");
    out.signed_number(signal.code);
    out.text("\n");
    out.text("address: ");
    out.hex(reinterpret_cast<u64>(signal.fault_address));
    out.text("\n");
    if (signal.detail != nullptr && signal.detail[0] != '\0') {
        out.line("detail", signal.detail);
    }
    out.text("last_frame: ");
    out.number(trace_last_frame());
    out.text("\n");
    out.text("monotonic_ns: ");
    out.number(monotonic_now_ns());
    out.text("\n");
}

/// Append a literal to a fixed buffer, tracking the length. Truncates rather than overflowing.
void append_text(char* out, u32 capacity, u32& length, const char* text) noexcept {
    for (const char* c = text; *c != '\0' && length + 1 < capacity; ++c) {
        out[length++] = *c;
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

}  // namespace

u64 write_crash_report_to_fd(i32 handle, const CrashSignal& signal) noexcept {
    SafeStream out(handle);
    const CrashState& crash = state();
    write_identity(out, crash);
    write_fault(out, signal);
    write_breadcrumbs(out);
    out.text("\n[backtrace]\n");
    const u32 frames = platform_write_backtrace(out.handle());
    if (frames == 0) {
        out.text("  <no backtrace available on this platform>\n");
    }
    out.text("\n[end]\n");
    return out.bytes();
}

Expected<u64, cy::Error> write_crash_report(const CrashSignal& signal) noexcept {
    CrashState& crash = state();
    if (crash.path[0] == '\0') {
        return fail(ErrorCode::Unavailable,
                    "no crash handler is installed, so no path is prepared");
    }
    const i32 handle = platform_create_file(crash.path);
    if (handle < 0) {
        return fail(ErrorCode::Io, "the crash report could not be created");
    }
    const u64 bytes = write_crash_report_to_fd(handle, signal);
    platform_close_file(handle);
    return bytes;
}

const char* crash_report_path() noexcept {
    return state().path;
}

Expected<u32, cy::Error> install_crash_handler(const CrashConfig& config) noexcept {
    CrashState& crash = state();
    crash.engine_version = (config.engine_version != nullptr && config.engine_version[0] != '\0')
                               ? config.engine_version
                               : CY_DIAG_ENGINE_VERSION;
    crash.build_identity = (config.build_identity != nullptr) ? config.build_identity : "";
    crash.policy = config.policy;

    char directory[kPathCapacity] = {};
    if (config.directory != nullptr && config.directory[0] != '\0') {
        std::strncpy(directory, config.directory, kPathCapacity - 1);
    } else {
        platform_default_crash_directory(directory, kPathCapacity);
    }
    if (!platform_make_directories(directory)) {
        return fail(ErrorCode::Io, "the crash directory could not be created");
    }

    // Composed now, not at fault time: the handler opens a path it was handed and formats nothing.
    std::memset(crash.path, 0, sizeof(crash.path));
    u32 length = 0;
    append_text(crash.path, kPathCapacity, length, directory);
    append_text(crash.path, kPathCapacity, length, "/crash-");
    append_decimal(crash.path, kPathCapacity, length, platform_process_id());
    append_text(crash.path, kPathCapacity, length, "-");
    append_decimal(crash.path, kPathCapacity, length, monotonic_now_ns());
    append_text(crash.path, kPathCapacity, length, ".txt");

    const u32 taken = platform_install_crash_handler();
    crash.installed.store(true, std::memory_order_release);
    return taken;
}

void uninstall_crash_handler() noexcept {
    platform_uninstall_crash_handler();
    state().installed.store(false, std::memory_order_release);
}

}  // namespace cy::diag
