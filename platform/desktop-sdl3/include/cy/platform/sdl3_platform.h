// The SDL3 Platform for Linux, Windows and macOS. Tasks 3.2.2, 3.2.3 and 3.2.4.
//
// design.md §4: SDL3 implements Platform, DisplayServer and the input event source for all three
// desktop platforms, and a native backend for one of them is a scheduled M11 task rather than a
// hope. No SDL type appears in this header, or anywhere above platform/ — tools/layercheck/ fails
// the build if one does.
//
// Not everything the interface promises is in SDL3. The executable path, the crash handler and the
// per-process memory figures are host services, and they live in one file per operating system
// under src/host/, selected by CMakeLists.txt. There is no #ifdef in this backend's shared files;
// that is task 3.2.4, and it is the rule that keeps a fourth platform a matter of adding a file.

#pragma once

#include <cy/core/base/types.h>
#include <cy/core/platform/platform.h>

namespace cy {

class Sdl3Platform final : public Platform {
public:
    // Concurrent subprocesses. A build tool run, a shader compile, an asset import; the number is
    // generous for M0 and the limit is reported rather than silently exceeded.
    static constexpr usize kMaxProcesses = 32;

    Sdl3Platform() = default;
    ~Sdl3Platform() override;

    // argc and argv as main() received them; they must outlive this object, which they do, because
    // they are the process's own. Initialising twice is an error rather than a silent reset.
    Status initialise(int argument_count, char** arguments);
    void shutdown();

    [[nodiscard]] std::string_view name() const override { return "desktop-sdl3"; }

    void request_exit(i32 exit_code) override;
    [[nodiscard]] bool exit_requested() const override { return exit_requested_; }
    [[nodiscard]] i32 exit_code() const override { return exit_code_; }

    [[nodiscard]] usize argument_count() const override { return argument_count_; }
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

    [[nodiscard]] Nanoseconds monotonic_nanoseconds() const override;
    [[nodiscard]] i64 wall_nanoseconds() const override;

    Expected<usize, Error> locale(char* buffer, usize capacity) const override;

    [[nodiscard]] u32 cpu_count() const override;
    [[nodiscard]] CpuFeatures cpu_features() const override;

    Expected<MemoryStatistics, Error> memory_statistics() const override;

    Status install_crash_handler(CrashHandler handler, void* user) override;
    void uninstall_crash_handler() override;

    // The organisation and application names the user directories are derived from. Set before
    // initialise(); the defaults are the engine's own, which is what a sample or a test wants.
    void set_application_identity(const char* organisation, const char* application);

private:
    struct ProcessSlot {
        ProcessHandle handle = 0;
        // The SDL_Process*, held as void* so that no SDL type reaches this header.
        void* process = nullptr;
        bool exited = false;
        i32 exit_code = 0;
    };

    ProcessSlot* find_process(ProcessHandle process);

    // SDL_GetPrefPath() returns one directory. Config and cache are derived from it per platform
    // convention; the helper is where that derivation lives.
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
