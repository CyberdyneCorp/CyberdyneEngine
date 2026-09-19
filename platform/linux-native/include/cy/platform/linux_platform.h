// The native Linux Platform — POSIX and glibc, with no SDL3 anywhere beneath it. M11.d task 4.1.
//
// WHY THIS EXISTS AT ALL. `core-platform-abstraction` has had exactly one implementation of
// `Platform` and one of `DisplayServer` since M0, and both are SDL3's. An interface implemented
// once is an interface nobody has tested: every assumption SDL3 happens to satisfy is invisible
// until a second implementation refuses it. This is that second implementation, and the finding it
// produces is worth more than the code — see platform/linux-native/README.md, which records what
// the port could and could not do without changing a line above layer 3.
//
// It is NOT a replacement. `platform/desktop-sdl3/` stays, stays tested and stays the default; two
// implementations is the point, and deleting the first would leave the abstraction proved by
// exactly as many implementations as before.
//
// WHAT IS NATIVE HERE. Everything: process lifetime, the argument vector, the environment, the
// standard streams, the XDG user directories, /proc/self/exe, dlopen, fork/exec with pipes,
// clock_gettime, setlocale, sysconf, /proc/meminfo and sigaction. No library is linked for any of
// it. The display side is X11 and lives in x11_display_server.h, because `Platform` and
// `DisplayServer` are separate interfaces and a headless run needs only the first.
//
// THE BUFFER CONVENTION IS THE INTERFACE'S, NOT THIS FILE'S. Every text-returning call takes a
// caller-owned buffer, never allocates, and answers ErrorCode::BufferTooSmall rather than
// truncating. cy/core/platform/platform.h states it once; this file obeys it.

#pragma once

#include <cy/core/base/types.h>
#include <cy/core/platform/platform.h>

namespace cy {

class LinuxPlatform final : public Platform {
public:
    // Concurrent subprocesses. The same figure `Sdl3Platform` carries, for the same reason: the
    // limit is reported rather than silently exceeded.
    static constexpr usize kMaxProcesses = 32;

    LinuxPlatform() = default;
    ~LinuxPlatform() override;

    LinuxPlatform(const LinuxPlatform&) = delete;
    LinuxPlatform& operator=(const LinuxPlatform&) = delete;

    /// argc and argv as `main()` received them; they must outlive this object, which they do,
    /// because they are the process's own. Initialising twice is an error rather than a silent
    /// reset — the same contract `Sdl3Platform::initialise()` has, because a caller that can swap
    /// the two must not have to read two contracts.
    Status initialise(int argument_count, char** arguments);
    void shutdown();

    [[nodiscard]] std::string_view name() const override { return "linux-native"; }

    void request_exit(i32 exit_code) override;
    [[nodiscard]] bool exit_requested() const override { return exit_requested_; }
    [[nodiscard]] i32 exit_code() const override { return exit_code_; }

    [[nodiscard]] usize argument_count() const override {
        return static_cast<usize>(argument_count_);
    }
    [[nodiscard]] std::string_view argument(usize index) const override;

    Expected<usize, Error> environment_variable(const char* name, char* buffer,
                                                usize capacity) const override;
    Status set_environment_variable(const char* name, const char* value) override;

    void write_standard_output(std::string_view text) override;
    void write_standard_error(std::string_view text) override;

    Expected<usize, Error> user_data_directory(char* buffer, usize capacity) const override;
    Expected<usize, Error> user_config_directory(char* buffer, usize capacity) const override;
    Expected<usize, Error> user_cache_directory(char* buffer, usize capacity) const override;
    Expected<usize, Error> executable_path(char* buffer, usize capacity) const override;

    Expected<LibraryHandle, Error> load_library(const char* path) override;
    Expected<void*, Error> library_symbol(LibraryHandle library, const char* symbol) override;
    void unload_library(LibraryHandle library) override;

    Expected<ProcessHandle, Error> spawn_process(const ProcessOptions& options) override;
    Expected<ProcessStatus, Error> poll_process(ProcessHandle process) override;
    Expected<i32, Error> wait_process(ProcessHandle process) override;
    Status terminate_process(ProcessHandle process, bool force) override;
    void release_process(ProcessHandle process) override;
    [[nodiscard]] Expected<i64, Error> process_id(ProcessHandle process) const override;
    Expected<usize, Error> write_process_input(ProcessHandle process,
                                               std::string_view bytes) override;
    Status close_process_input(ProcessHandle process) override;
    Expected<usize, Error> read_process_output(ProcessHandle process, char* buffer,
                                               usize capacity) override;

    [[nodiscard]] Nanoseconds monotonic_nanoseconds() const override;
    [[nodiscard]] i64 wall_nanoseconds() const override;

    Expected<usize, Error> locale(char* buffer, usize capacity) const override;

    [[nodiscard]] u32 cpu_count() const override;
    [[nodiscard]] CpuFeatures cpu_features() const override;

    Expected<MemoryStatistics, Error> memory_statistics() const override;

    Status install_crash_handler(CrashHandler handler, void* user) override;
    void uninstall_crash_handler() override;

    /// The organisation and application names the user directories are derived from. Set before
    /// initialise(). The defaults are the engine's own and are deliberately the SAME two strings
    /// `Sdl3Platform` defaults to, so that a run under this backend reads and writes the very
    /// directory a run under SDL3 did — a port that quietly moved the user's saves would be a port
    /// that passed every test and lost a player's game.
    void set_application_identity(const char* organisation, const char* application);

private:
    struct ProcessSlot {
        ProcessHandle handle = 0;
        i64 pid = 0;
        bool exited = false;
        i32 exit_code = 0;
        /// Whether this child was spawned with its standard streams on pipes. The three stream
        /// calls refuse on a child that was not, rather than writing into a descriptor that is the
        /// parent's own terminal.
        bool piped = false;
        bool input_closed = false;
        /// The parent's ends of the child's stdin and stdout, or -1. Raw descriptors rather than
        /// FILE*: the read has to be non-blocking, which is a property of the descriptor.
        int input_fd = -1;
        int output_fd = -1;
    };

    ProcessSlot* find_process(ProcessHandle process);
    const ProcessSlot* find_process(ProcessHandle process) const;
    /// Reaps the child if it has exited, recording its code. Non-blocking unless `block`.
    void reap(ProcessSlot& slot, bool block);
    void close_slot_descriptors(ProcessSlot& slot);

    /// The user data directory with `suffix` appended, creating every component. The three user
    /// directories are derived here and nowhere else.
    Expected<usize, Error> preference_path(const char* suffix, char* buffer, usize capacity) const;

    int argument_count_ = 0;
    char** arguments_ = nullptr;
    bool initialised_ = false;
    bool exit_requested_ = false;
    i32 exit_code_ = 0;

    const char* organisation_ = "CyberdyneEngine";
    const char* application_ = "CyberdyneEngine";

    ProcessSlot processes_[kMaxProcesses];
    ProcessHandle next_process_ = 1;
};

}  // namespace cy
