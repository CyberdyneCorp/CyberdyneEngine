// SPDX-License-Identifier: MIT
#import <Foundation/Foundation.h>
#import <mach/mach.h>

#include <cy/platform/ios_platform.h>

#include <chrono>
#include <cstdio>

namespace cy {
namespace {

Unexpected<Error> no_processes() {
    return fail(ErrorCode::Unsupported, "iOS applications cannot create or control subprocesses");
}

Expected<usize, Error> directory(NSSearchPathDirectory kind, char* buffer, usize capacity) {
    NSArray<NSString*>* paths = NSSearchPathForDirectoriesInDomains(kind, NSUserDomainMask, YES);
    if (paths.count == 0) {
        return fail(ErrorCode::Unavailable, "iOS did not provide the requested sandbox directory");
    }
    NSString* path = [paths.firstObject stringByAppendingString:@"/"];
    return write_to_buffer(buffer, capacity, path.UTF8String);
}

}  // namespace

void IosPlatform::request_exit(i32 code) {
    if (!exit_requested_) {
        exit_requested_ = true;
        exit_code_ = code;
    }
}

std::string_view IosPlatform::argument(usize) const { return {}; }

Expected<usize, Error> IosPlatform::environment_variable(const char*, char*, usize) const {
    return fail(ErrorCode::NotFound, "iOS application bundles do not expose an environment");
}

Status IosPlatform::set_environment_variable(const char*, const char*) {
    return fail(ErrorCode::Unsupported, "iOS application bundles cannot mutate their environment");
}

void IosPlatform::write_standard_output(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

void IosPlatform::write_standard_error(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
}

Expected<usize, Error> IosPlatform::user_data_directory(char* buffer, usize capacity) const {
    return directory(NSApplicationSupportDirectory, buffer, capacity);
}

Expected<usize, Error> IosPlatform::user_config_directory(char* buffer, usize capacity) const {
    return directory(NSLibraryDirectory, buffer, capacity);
}

Expected<usize, Error> IosPlatform::user_cache_directory(char* buffer, usize capacity) const {
    return directory(NSCachesDirectory, buffer, capacity);
}

Expected<usize, Error> IosPlatform::executable_path(char* buffer, usize capacity) const {
    NSString* path = NSBundle.mainBundle.executablePath;
    if (path == nil) {
        return fail(ErrorCode::Unavailable, "iOS bundle has no executable path");
    }
    return write_to_buffer(buffer, capacity, path.UTF8String);
}

Expected<LibraryHandle, Error> IosPlatform::load_library(const char*) {
    return fail(ErrorCode::Unsupported, "iOS forbids runtime-loaded native engine plugins");
}
Expected<void*, Error> IosPlatform::library_symbol(LibraryHandle, const char*) {
    return fail(ErrorCode::Unsupported, "iOS has no runtime-loaded native engine plugins");
}
Expected<ProcessHandle, Error> IosPlatform::spawn_process(const ProcessOptions&) { return no_processes(); }
Expected<ProcessStatus, Error> IosPlatform::poll_process(ProcessHandle) { return no_processes(); }
Expected<i32, Error> IosPlatform::wait_process(ProcessHandle) { return no_processes(); }
Status IosPlatform::terminate_process(ProcessHandle, bool) { return no_processes(); }
Expected<i64, Error> IosPlatform::process_id(ProcessHandle) const { return no_processes(); }
Expected<usize, Error> IosPlatform::write_process_input(ProcessHandle, std::string_view) { return no_processes(); }
Status IosPlatform::close_process_input(ProcessHandle) { return no_processes(); }
Expected<usize, Error> IosPlatform::read_process_output(ProcessHandle, char*, usize) { return no_processes(); }

Nanoseconds IosPlatform::monotonic_nanoseconds() const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

i64 IosPlatform::wall_nanoseconds() const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

Expected<usize, Error> IosPlatform::locale(char* buffer, usize capacity) const {
    NSString* language = NSLocale.preferredLanguages.firstObject;
    if (language == nil) {
        return fail(ErrorCode::Unavailable, "iOS did not provide a preferred language");
    }
    return write_to_buffer(buffer, capacity, language.UTF8String);
}

u32 IosPlatform::cpu_count() const {
    return static_cast<u32>(MAX((NSUInteger)1, NSProcessInfo.processInfo.activeProcessorCount));
}

CpuFeatures IosPlatform::cpu_features() const {
    CpuFeatures features;
#if defined(__aarch64__)
    features.neon = true;
#endif
    return features;
}

Expected<MemoryStatistics, Error> IosPlatform::memory_statistics() const {
    MemoryStatistics result;
    result.total_physical_bytes = NSProcessInfo.processInfo.physicalMemory;
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        result.process_resident_bytes = info.resident_size;
    }
    return result;
}

Status IosPlatform::install_crash_handler(CrashHandler, void*) {
    return fail(ErrorCode::Unsupported, "iOS owns native crash capture for application bundles");
}

}  // namespace cy
