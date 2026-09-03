// The host services SDL3 does not provide. Task 3.2.4.
//
// Three things the Platform interface promises have no SDL3 API: the path of the running
// executable, the per-process memory figures, and crash handler installation. Each is implemented
// once per operating system, in host_linux.cpp, host_windows.cpp and host_macos.cpp, and
// CMakeLists.txt selects the one that matches CMAKE_SYSTEM_NAME.
//
// That is the whole of the rule `core-platform-abstraction` states and task 3.2.4 restates:
// platform-specific code lives in a file of its own, never behind an #ifdef in a shared one. A
// fourth desktop platform is a fourth file and one line of CMake, and nothing already written is
// touched.

#pragma once

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/platform/display_server.h>
#include <cy/core/platform/platform.h>

struct SDL_Window;

namespace cy::host {

// The absolute path of the running executable, including its file name. SDL_GetBasePath() gives the
// directory it sits in, which is not the same answer.
Expected<usize, Error> executable_path(char* buffer, usize capacity);

// Fills in the two figures SDL_GetSystemRAM() does not report: physical memory currently available,
// and this process's resident set. Leaves a figure at zero — "unknown" — where the host does not
// offer it, rather than failing the whole call.
Status refine_memory_statistics(MemoryStatistics& statistics);

// Installs the process-wide crash handler: POSIX signals, or the Windows unhandled-exception
// filter. The handler runs on the crashing thread and may call only async-signal-safe operations;
// what it writes is task 3.5.8's, in src/core/diagnostics/.
Status install_crash_handler(CrashHandler handler, void* user);
void uninstall_crash_handler();

// The native window and display behind an SDL window, for NativeSurface. Returns false when SDL is
// running on a video driver this host file does not know — the offscreen and dummy drivers, for
// instance, which have no native window at all.
bool query_native_window(SDL_Window* window, NativeSurface& surface);

}  // namespace cy::host
