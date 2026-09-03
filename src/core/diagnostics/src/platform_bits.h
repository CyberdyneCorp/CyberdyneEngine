#pragma once
// The handful of operating-system calls diagnostics cannot avoid, declared once and implemented per
// platform in crash_handler_posix.cpp and crash_handler_windows.cpp.
//
// `core-platform-abstraction` puts these behind `Platform` (task 3.2.1), and that is where they
// belong. That interface did not exist when this module was written, so the implementations are two
// translation units selected by the build — never an #ifdef in a shared file, which is the rule
// src/core/README.md states.
//
// Everything here is callable from a signal handler: a raw descriptor write, a directory creation
// performed at installation rather than at fault time, and a backtrace writer that the platform
// documents as async-signal-safe.

#include <cy/core/diagnostics/crash.h>
#include <cy/core/diagnostics/prelude.h>

namespace cy::diag {

u32 platform_process_id() noexcept;

/// Create `path` and its parents. False when it cannot be created.
bool platform_make_directories(const char* path) noexcept;

/// The user mount's crash directory, written into `buffer`. $CY_CRASH_DIR wins; then the platform's
/// per-user state directory; then the working directory.
void platform_default_crash_directory(char* buffer, u32 capacity) noexcept;

/// Create the report file, truncating an existing one. A negative return means it could not be
/// opened; the caller reports that rather than allocating a fallback.
i32 platform_create_file(const char* path) noexcept;

/// Create the report file only if it does not exist. The fault handler uses this: an artefact a
/// fatal assertion already wrote says more than the SIGABRT that assertion raised a moment later,
/// and the first report of a dying process is the one worth keeping.
i32 platform_create_file_new(const char* path) noexcept;
void platform_close_file(i32 handle) noexcept;
/// Bytes written, or a negative value. Async-signal-safe: one raw write, no buffering, no locks.
i64 platform_write(i32 handle, const void* data, usize bytes) noexcept;

/// Write the calling thread's backtrace, one frame per line, with module identities and offsets.
/// Returns the number of frames, or zero where the platform gives none.
u32 platform_write_backtrace(i32 handle) noexcept;

/// Take over the process's fault conditions. Returns how many were taken.
u32 platform_install_crash_handler() noexcept;
void platform_uninstall_crash_handler() noexcept;

/// The name of a signal or exception code, as a literal.
const char* platform_fault_name(i32 number) noexcept;

}  // namespace cy::diag
