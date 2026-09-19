// SPDX-License-Identifier: MIT
// The stub platform's process services. See stub_platform.h for what it is for.
//
// EVERY REFUSAL BELOW IS ErrorCode::Unsupported WITH A SENTENCE. `Unsupported` rather than
// `NotImplemented`: the second says "later" and the first says "not on this target", and a caller
// that has to decide whether to wait for a feature or to do without it needs the difference.

#include <cy/platform/stub_platform.h>

#include <chrono>
#include <cstdio>
#include <cstring>

namespace cy {
namespace {

Unexpected<Error> no_processes() {
    return fail(ErrorCode::Unsupported,
                "the stub platform has no process table: it cannot spawn, poll, wait for, signal "
                "or talk to a child");
}

}  // namespace

void StubPlatform::set_user_mount(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    const usize length = std::strlen(path);
    // The interface requires every user directory to end in the platform's separator, and a caller
    // that forgot one would otherwise produce "…/stub-usersave.dat".
    const bool separated = path[length - 1] == '/';
    std::snprintf(user_mount_, sizeof(user_mount_), "%s%s", path, separated ? "" : "/");
}

void StubPlatform::request_exit(i32 exit_code) {
    if (!exit_requested_) {
        exit_requested_ = true;
        exit_code_ = exit_code;
    }
}

std::string_view StubPlatform::argument(usize /*index*/) const {
    return {};
}

Expected<usize, Error> StubPlatform::environment_variable(const char* /*name*/, char* /*buffer*/,
                                                          usize /*capacity*/) const {
    // Not `Unsupported`: an environment that exists and is empty is indistinguishable from one that
    // does not, and every caller of this already handles "no such variable".
    return fail(ErrorCode::NotFound, "the stub platform has no environment");
}

Status StubPlatform::set_environment_variable(const char* /*name*/, const char* /*value*/) {
    return fail(ErrorCode::Unsupported, "the stub platform has no environment to set");
}

void StubPlatform::write_standard_output(std::string_view text) {
    // A stub platform still has somewhere to say things. A target whose only log is a debug channel
    // implements exactly this call differently and nothing else changes.
    std::fwrite(text.data(), 1, text.size(), stdout);
}

void StubPlatform::write_standard_error(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
}

Expected<usize, Error> StubPlatform::user_data_directory(char* buffer, usize capacity) const {
    return write_to_buffer(buffer, capacity, user_mount_);
}

Expected<usize, Error> StubPlatform::user_config_directory(char* buffer, usize capacity) const {
    return write_to_buffer(buffer, capacity, user_mount_);
}

Expected<usize, Error> StubPlatform::user_cache_directory(char* buffer, usize capacity) const {
    return write_to_buffer(buffer, capacity, user_mount_);
}

Expected<usize, Error> StubPlatform::executable_path(char* /*buffer*/, usize /*capacity*/) const {
    return fail(ErrorCode::Unsupported,
                "the stub platform's image is not a file it can name; anything that resolves an "
                "asset root relative to the executable has to be told where it is instead");
}

Expected<LibraryHandle, Error> StubPlatform::load_library(const char* /*path*/) {
    return fail(ErrorCode::Unsupported,
                "the stub platform has no dynamic loader; a plugin is linked in or it is absent");
}

Expected<void*, Error> StubPlatform::library_symbol(LibraryHandle /*library*/,
                                                    const char* /*symbol*/) {
    return fail(ErrorCode::Unsupported, "the stub platform has no dynamic loader");
}

void StubPlatform::unload_library(LibraryHandle /*library*/) {}

Expected<ProcessHandle, Error> StubPlatform::spawn_process(const ProcessOptions& /*options*/) {
    return no_processes();
}

Expected<ProcessStatus, Error> StubPlatform::poll_process(ProcessHandle /*process*/) {
    return no_processes();
}

Expected<i32, Error> StubPlatform::wait_process(ProcessHandle /*process*/) {
    return no_processes();
}

Status StubPlatform::terminate_process(ProcessHandle /*process*/, bool /*force*/) {
    return no_processes();
}

void StubPlatform::release_process(ProcessHandle /*process*/) {}

Expected<i64, Error> StubPlatform::process_id(ProcessHandle /*process*/) const {
    return no_processes();
}

Expected<usize, Error> StubPlatform::write_process_input(ProcessHandle /*process*/,
                                                         std::string_view /*bytes*/) {
    return no_processes();
}

Status StubPlatform::close_process_input(ProcessHandle /*process*/) {
    return no_processes();
}

Expected<usize, Error> StubPlatform::read_process_output(ProcessHandle /*process*/,
                                                         char* /*buffer*/, usize /*capacity*/) {
    return no_processes();
}

Nanoseconds StubPlatform::monotonic_nanoseconds() const {
    const auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
}

i64 StubPlatform::wall_nanoseconds() const {
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
}

Expected<usize, Error> StubPlatform::locale(char* /*buffer*/, usize /*capacity*/) const {
    return fail(ErrorCode::Unavailable,
                "the stub platform has no user and therefore no preferred locale; a caller falls "
                "back to its own default rather than to a language it guessed");
}

Expected<MemoryStatistics, Error> StubPlatform::memory_statistics() const {
    MemoryStatistics statistics;
    // A small, FIXED budget, and both figures are the same one. A target that reports the memory it
    // was given rather than the memory the machine has is the ordinary case off the desktop, and a
    // subsystem that sizes a pool as a fraction of "total" has to survive it being 512 MiB.
    statistics.total_physical_bytes = 512ULL * 1024ULL * 1024ULL;
    statistics.available_physical_bytes = statistics.total_physical_bytes;
    // Zero means unknown, and a platform with no process accounting genuinely does not know.
    statistics.process_resident_bytes = 0;
    return statistics;
}

Status StubPlatform::install_crash_handler(CrashHandler /*handler*/, void* /*user*/) {
    return fail(ErrorCode::Unsupported,
                "the stub platform installs no crash handler; the report a handler would write is "
                "produced by the host's own crash reporter on a target like this one");
}

void StubPlatform::uninstall_crash_handler() {}

}  // namespace cy
