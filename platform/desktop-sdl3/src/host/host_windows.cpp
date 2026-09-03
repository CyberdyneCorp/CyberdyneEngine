// Windows host services. See host.h; CMakeLists.txt compiles this file only on Windows.
//
// UNVERIFIED. Written against the documented Win32 surface and reviewed, but M0 was developed on
// Linux and no Windows host was available to run it. The first Windows CI job (task 2.4.1) is what
// turns this comment into a fact.

#include "host.h"

#include <SDL3/SDL.h>

#include <iterator>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>

namespace cy::host {

Expected<usize, Error> executable_path(char* buffer, usize capacity) {
    // Wide first, then UTF-8: the engine's text is UTF-8 everywhere, and a path with a non-ANSI
    // character would be mangled by the narrow entry point.
    wchar_t wide[MAX_PATH * 4];
    const DWORD length = ::GetModuleFileNameW(nullptr, wide, static_cast<DWORD>(std::size(wide)));
    if (length == 0 || length == std::size(wide)) {
        return fail(ErrorCode::Unavailable, "GetModuleFileNameW() did not return a path");
    }

    char utf8[4096];
    const int written = ::WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length), utf8,
                                              static_cast<int>(sizeof(utf8)), nullptr, nullptr);
    if (written <= 0) {
        return fail(ErrorCode::Internal, "the executable path is not representable as UTF-8");
    }
    return write_to_buffer(buffer, capacity, std::string_view{utf8, static_cast<usize>(written)});
}

Status refine_memory_statistics(MemoryStatistics& statistics) {
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (::GlobalMemoryStatusEx(&memory) != 0) {
        statistics.available_physical_bytes = static_cast<u64>(memory.ullAvailPhys);
    }

    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)) != 0) {
        statistics.process_resident_bytes = static_cast<u64>(counters.WorkingSetSize);
    }
    return ok();
}

bool query_native_window(SDL_Window* window, NativeSurface& surface) {
    if (window == nullptr) {
        return false;
    }
    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    if (properties == 0) {
        return false;
    }
    void* hwnd = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (hwnd == nullptr) {
        return false;
    }
    surface.handle = hwnd;
    // Windows has no display object; the HWND is the whole of what a backend needs.
    surface.display = nullptr;
    return true;
}

}  // namespace cy::host
