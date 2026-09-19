// SPDX-License-Identifier: MIT
#pragma once
// The stub platform: the porting surface's own proof. M11.d task 4.4.
//
// ================================================================================================
// WHAT IT IS FOR
// ================================================================================================
//
// `platform/desktop-sdl3/` and `platform/linux-native/` are both DESKTOPS. They disagree about
// which library opens a window and agree about everything a desktop takes for granted: a command
// line, a mouse, a resizable window, several writable directories, a process table, a dynamic
// loader, many cores. A porting surface exercised only by desktops is a desktop interface, and no
// amount of desktop backends discovers that.
//
// So this one shares no desktop assumption at all. It answers **false to every `Feature`**, has
// **one** window that is **not resizable**, **one** writable directory and no others, **no**
// command line, **no** subprocesses, **no** dynamic libraries, **no** crash handler, **one** core
// and **no** SIMD. Everything it refuses, it refuses with a reason, because the interface's whole
// contract is that a capability is QUERIED and a refusal is reported rather than assumed.
//
// It is not a mock and it is not a test double: it is a real implementation of both interfaces, it
// builds and links into the engine like any other port, and `integration.stub_platform` runs actual
// frames through `Runtime::tick()` on it. What makes it a stub is the smallness of what it offers,
// not the honesty of what it does.
//
// ================================================================================================
// IT DOES NOT OWN THE MAIN LOOP
// ================================================================================================
//
// `core-platform-abstraction` requires the loop to be the platform's and not the engine's, and
// `platform/host/` is the desktop's answer — a `while (running)` that calls `Runtime::tick()`. A
// platform that drives frames itself (a console's presentation callback, a browser's animation
// frame) calls `tick()` from wherever its own loop lives and changes nothing else. This stub is on
// that side of the line: it exposes `request_exit()` and never calls back into the runtime, so the
// caller's loop — `run_host_loop()` or its own — is what advances frames. The test drives it with a
// plain `for`, which is the cheapest possible demonstration that the engine does not need the host
// loop to exist.

#include <cy/core/base/types.h>
#include <cy/core/platform/platform.h>

namespace cy {

class StubPlatform final : public Platform {
public:
    StubPlatform() = default;
    ~StubPlatform() override = default;

    StubPlatform(const StubPlatform&) = delete;
    StubPlatform& operator=(const StubPlatform&) = delete;

    [[nodiscard]] std::string_view name() const override { return "stub"; }

    /// The one directory this platform may write to. Every one of the three user directories
    /// answers it — see user_data_directory(). It ends in a separator, as the interface requires.
    void set_user_mount(const char* path);

    void request_exit(i32 exit_code) override;
    [[nodiscard]] bool exit_requested() const override { return exit_requested_; }
    [[nodiscard]] i32 exit_code() const override { return exit_code_; }

    /// ZERO, ALWAYS. A stub platform has no command line: an application bundle launched by a
    /// window server and a title booted from a disc both get none. Code that reads its
    /// configuration only from `argv` does not run here, and finding that out is the point.
    [[nodiscard]] usize argument_count() const override { return 0; }
    [[nodiscard]] std::string_view argument(usize index) const override;

    Expected<usize, Error> environment_variable(const char* name, char* buffer,
                                                usize capacity) const override;
    Status set_environment_variable(const char* name, const char* value) override;

    void write_standard_output(std::string_view text) override;
    void write_standard_error(std::string_view text) override;

    /// ALL THREE ANSWER THE SAME DIRECTORY, and that is the assumption being broken. A desktop has
    /// a data directory, a config directory and a cache directory and they are distinct; a target
    /// with one writable mount has one. Code that relies on being able to fill the cache without
    /// touching the save data behaves differently here.
    Expected<usize, Error> user_data_directory(char* buffer, usize capacity) const override;
    Expected<usize, Error> user_config_directory(char* buffer, usize capacity) const override;
    Expected<usize, Error> user_cache_directory(char* buffer, usize capacity) const override;
    /// Unsupported: the running image need not be a file this process can name.
    Expected<usize, Error> executable_path(char* buffer, usize capacity) const override;

    /// All three unsupported. A platform with no dynamic loader is not hypothetical — it is every
    /// console and every iOS build — and the engine's plugin story has to survive being told so.
    Expected<LibraryHandle, Error> load_library(const char* path) override;
    Expected<void*, Error> library_symbol(LibraryHandle library, const char* symbol) override;
    void unload_library(LibraryHandle library) override;

    /// Every one unsupported: there is no process table to put a child in.
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

    /// REAL, BOTH OF THEM. A clock is the one service no target can be without and no engine can
    /// simulate away: a platform that answered zero here would make every frame take no time and
    /// every test that measures one vacuous.
    [[nodiscard]] Nanoseconds monotonic_nanoseconds() const override;
    [[nodiscard]] i64 wall_nanoseconds() const override;

    Expected<usize, Error> locale(char* buffer, usize capacity) const override;

    /// ONE core and NO instruction-set extensions. Both are the interesting answer rather than the
    /// lazy one: a jobs system that needs two workers and a dispatch that assumes SSE 4.2 both fail
    /// here and nowhere else in this tree.
    [[nodiscard]] u32 cpu_count() const override { return 1; }
    [[nodiscard]] CpuFeatures cpu_features() const override { return CpuFeatures{}; }

    Expected<MemoryStatistics, Error> memory_statistics() const override;

    Status install_crash_handler(CrashHandler handler, void* user) override;
    void uninstall_crash_handler() override;

private:
    bool exit_requested_ = false;
    i32 exit_code_ = 0;
    char user_mount_[256] = "stub-user/";
};

}  // namespace cy
