// SPDX-License-Identifier: MIT
#pragma once

#include <cy/core/platform/platform.h>

namespace cy {

/// iOS process services. UIKit owns lifetime and the frame loop; this object exposes only the
/// services an application sandbox actually provides.
class IosPlatform final : public Platform {
public:
    [[nodiscard]] std::string_view name() const override { return "ios"; }

    void request_exit(i32 exit_code) override;
    [[nodiscard]] bool exit_requested() const override { return exit_requested_; }
    [[nodiscard]] i32 exit_code() const override { return exit_code_; }

    [[nodiscard]] usize argument_count() const override { return 0; }
    [[nodiscard]] std::string_view argument(usize index) const override;
    Expected<usize, Error> environment_variable(const char*, char*, usize) const override;
    Status set_environment_variable(const char*, const char*) override;
    void write_standard_output(std::string_view text) override;
    void write_standard_error(std::string_view text) override;

    Expected<usize, Error> user_data_directory(char*, usize) const override;
    Expected<usize, Error> user_config_directory(char*, usize) const override;
    Expected<usize, Error> user_cache_directory(char*, usize) const override;
    Expected<usize, Error> executable_path(char*, usize) const override;

    Expected<LibraryHandle, Error> load_library(const char*) override;
    Expected<void*, Error> library_symbol(LibraryHandle, const char*) override;
    void unload_library(LibraryHandle) override {}
    Expected<ProcessHandle, Error> spawn_process(const ProcessOptions&) override;
    Expected<ProcessStatus, Error> poll_process(ProcessHandle) override;
    Expected<i32, Error> wait_process(ProcessHandle) override;
    Status terminate_process(ProcessHandle, bool) override;
    void release_process(ProcessHandle) override {}
    [[nodiscard]] Expected<i64, Error> process_id(ProcessHandle) const override;
    Expected<usize, Error> write_process_input(ProcessHandle, std::string_view) override;
    Status close_process_input(ProcessHandle) override;
    Expected<usize, Error> read_process_output(ProcessHandle, char*, usize) override;

    [[nodiscard]] Nanoseconds monotonic_nanoseconds() const override;
    [[nodiscard]] i64 wall_nanoseconds() const override;
    Expected<usize, Error> locale(char*, usize) const override;
    [[nodiscard]] u32 cpu_count() const override;
    [[nodiscard]] CpuFeatures cpu_features() const override;
    Expected<MemoryStatistics, Error> memory_statistics() const override;
    Status install_crash_handler(CrashHandler, void*) override;
    void uninstall_crash_handler() override {}

private:
    bool exit_requested_ = false;
    i32 exit_code_ = 0;
};

}  // namespace cy
