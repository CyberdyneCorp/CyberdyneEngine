// The Platform interface. Task 3.2.1.
//
// `core-platform-abstraction` fixes the surface: "process lifetime and exit code, command-line
// arguments, environment variables, standard output and error, the user data / config / cache
// directories, the executable path, dynamic library loading and symbol resolution, subprocess
// creation and control, monotonic and wall clocks, locale, CPU count and features, memory
// statistics, and crash handler installation". Everything below is one of those, and nothing below
// is anything else.
//
// This is the interface only. Implementations live under platform/<name>/ — there is no host
// #ifdef anywhere in this directory, and tools/layercheck/ keeps it that way. A new port implements
// this class, DisplayServer, an input backend, an audio backend and a surface provider, "with no
// changes required in src/core/ or above".
//
// TEXT-RETURNING CALLS take a caller-owned buffer and return the number of bytes written, not
// counting the terminating NUL. They never allocate: there are no allocators at M0 (design.md §9),
// and a path or an environment variable is the wrong reason to introduce one. A buffer too small
// for the answer is ErrorCode::BufferTooSmall, and the value is not truncated into it.

#pragma once

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <string_view>

namespace cy {

// An opaque handle to a loaded shared object. Null is never a valid loaded library.
using LibraryHandle = void*;

// An opaque handle to a spawned process. Zero is never a valid process.
using ProcessHandle = u64;

struct ProcessOptions {
    // NUL-terminated argument vector, argv[0] first. Not copied: it must outlive the spawn call.
    const char* const* arguments = nullptr;
    usize argument_count = 0;
    // Null means the calling process's directory.
    const char* working_directory = nullptr;
    // False redirects the child's standard streams to the platform's null device.
    bool inherit_standard_streams = true;
};

struct ProcessStatus {
    bool running = true;
    // Meaningful only when `running` is false.
    i32 exit_code = 0;
};

// The instruction-set extensions the engine dispatches on. Reported, never assumed: a build may be
// compiled for a baseline older than the machine it runs on.
struct CpuFeatures {
    bool sse42 = false;
    bool avx = false;
    bool avx2 = false;
    bool avx512f = false;
    bool neon = false;
};

struct MemoryStatistics {
    u64 total_physical_bytes = 0;
    // Zero when the platform does not report it; a caller treats it as "unknown", not as "none".
    u64 available_physical_bytes = 0;
    u64 process_resident_bytes = 0;
};

// What the operating system delivered. `signal_or_exception` is the POSIX signal number or the
// Windows exception code; `description` is the platform's own name for it and is never null.
struct CrashContext {
    i32 signal_or_exception = 0;
    const char* description = "";
};

// Runs on the crashing thread, inside a signal or exception context: it may call only
// async-signal-safe operations. The body belongs to `diagnostics-profiling-and-crash` (task 3.5.8),
// which writes the report; this interface installs it and nothing more.
using CrashHandler = void (*)(const CrashContext& context, void* user);

class Platform {
public:
    Platform() = default;
    virtual ~Platform() = default;

    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;

    // The implementation's own name — "desktop-sdl3", "headless" — for a diagnostic.
    [[nodiscard]] virtual std::string_view name() const = 0;

    // --- Process lifetime and exit code -----------------------------------------------------
    //
    // Requesting exit records the intent; it does not end the process. The host's loop observes it
    // and returns from main(), because a platform that drives frames itself is not free to exit
    // from inside one (`core-platform-abstraction`, "Platform does not own the main loop").

    virtual void request_exit(i32 exit_code) = 0;
    [[nodiscard]] virtual bool exit_requested() const = 0;
    [[nodiscard]] virtual i32 exit_code() const = 0;

    // --- Command-line arguments -------------------------------------------------------------

    [[nodiscard]] virtual usize argument_count() const = 0;
    // Empty for an index at or past argument_count().
    [[nodiscard]] virtual std::string_view argument(usize index) const = 0;

    // --- Environment ------------------------------------------------------------------------

    virtual Expected<usize, Error> environment_variable(const char* name, char* buffer,
                                                        usize capacity) const = 0;
    virtual Status set_environment_variable(const char* name, const char* value) = 0;

    // --- Standard output and error ----------------------------------------------------------
    //
    // Bytes, not lines: no newline is added and nothing is formatted. The text is UTF-8 on every
    // platform, including Windows, where the implementation converts.

    virtual void write_standard_output(std::string_view text) = 0;
    virtual void write_standard_error(std::string_view text) = 0;

    // --- Directories and the executable -----------------------------------------------------
    //
    // The three user directories are the only paths the engine may write to: a planned mobile
    // target has no writable filesystem outside them. Each ends in the platform's separator.

    virtual Expected<usize, Error> user_data_directory(char* buffer, usize capacity) const = 0;
    virtual Expected<usize, Error> user_config_directory(char* buffer, usize capacity) const = 0;
    virtual Expected<usize, Error> user_cache_directory(char* buffer, usize capacity) const = 0;
    virtual Expected<usize, Error> executable_path(char* buffer, usize capacity) const = 0;

    // --- Dynamic libraries ------------------------------------------------------------------

    virtual Expected<LibraryHandle, Error> load_library(const char* path) = 0;
    virtual Expected<void*, Error> library_symbol(LibraryHandle library, const char* symbol) = 0;
    virtual void unload_library(LibraryHandle library) = 0;

    // --- Subprocesses -----------------------------------------------------------------------

    virtual Expected<ProcessHandle, Error> spawn_process(const ProcessOptions& options) = 0;
    // Non-blocking: reports whether the child is still running, and its code once it is not.
    virtual Expected<ProcessStatus, Error> poll_process(ProcessHandle process) = 0;
    // Blocks until the child exits and returns its exit code.
    virtual Expected<i32, Error> wait_process(ProcessHandle process) = 0;
    virtual Status terminate_process(ProcessHandle process, bool force) = 0;
    // Releases the handle. A process that is still running is not killed by this.
    virtual void release_process(ProcessHandle process) = 0;

    // --- Clocks -----------------------------------------------------------------------------
    //
    // Two clocks, two sources, and the specification's reason for the split: "the system clock is
    // adjusted while the game runs → frame timing SHALL be unaffected". Frame timing reads
    // monotonic_nanoseconds() and nothing else. Its zero is arbitrary and only differences of two
    // readings mean anything; it never goes backwards and it is not affected by the wall clock.

    [[nodiscard]] virtual Nanoseconds monotonic_nanoseconds() const = 0;
    // Nanoseconds since the Unix epoch. It can jump in either direction — an NTP correction, a
    // daylight-saving change, a user editing the clock — so nothing that has to advance smoothly
    // may be derived from it.
    [[nodiscard]] virtual i64 wall_nanoseconds() const = 0;

    // --- Locale, CPU and memory -------------------------------------------------------------

    // The user's preferred locale as a BCP-47 tag: "en-GB", "pt-BR".
    virtual Expected<usize, Error> locale(char* buffer, usize capacity) const = 0;

    // Logical processors, including SMT siblings. Never zero: an implementation that cannot tell
    // reports 1.
    [[nodiscard]] virtual u32 cpu_count() const = 0;
    [[nodiscard]] virtual CpuFeatures cpu_features() const = 0;

    virtual Expected<MemoryStatistics, Error> memory_statistics() const = 0;

    // --- Crash handling ---------------------------------------------------------------------
    //
    // Installation only. What the handler writes — the signal, the symbolised backtrace, the engine
    // version and the last logged frame — is task 3.5.8's, in src/core/diagnostics/.

    virtual Status install_crash_handler(CrashHandler handler, void* user) = 0;
    virtual void uninstall_crash_handler() = 0;
};

// Copies `text` and its NUL into `buffer`, returning the length written without the NUL. The shared
// implementation of the buffer convention described at the top of this file; every text-returning
// call above ends in it.
Expected<usize, Error> write_to_buffer(char* buffer, usize capacity, std::string_view text);

}  // namespace cy
