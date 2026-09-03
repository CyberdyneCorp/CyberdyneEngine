// Linux host services. See host.h; CMakeLists.txt compiles this file only on Linux.

#include "host.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <unistd.h>

namespace cy::host {
namespace {

// One line of /proc/meminfo, as bytes. The file reports kibibytes; a key that is absent is
// reported as zero, which is what "unknown" means to MemoryStatistics.
u64 read_meminfo_bytes(const char* key) {
    std::FILE* file = std::fopen("/proc/meminfo", "re");
    if (file == nullptr) {
        return 0;
    }

    const usize key_length = std::strlen(key);
    u64 bytes = 0;
    char line[256];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, key, key_length) != 0 || line[key_length] != ':') {
            continue;
        }
        const char* value = line + key_length + 1;
        char* end = nullptr;
        const unsigned long long kibibytes = std::strtoull(value, &end, 10);
        if (end != value) {
            bytes = static_cast<u64>(kibibytes) * 1024ULL;
        }
        break;
    }
    std::fclose(file);
    return bytes;
}

u64 read_resident_bytes() {
    std::FILE* file = std::fopen("/proc/self/statm", "re");
    if (file == nullptr) {
        return 0;
    }
    // /proc/self/statm: total pages, then resident pages. Both in pages of _SC_PAGESIZE.
    char line[128] = {};
    const bool read = std::fgets(line, sizeof(line), file) != nullptr;
    std::fclose(file);
    if (!read) {
        return 0;
    }
    char* after_total = nullptr;
    (void)std::strtoull(line, &after_total, 10);
    if (after_total == line) {
        return 0;
    }
    char* after_resident = nullptr;
    const unsigned long long resident_pages = std::strtoull(after_total, &after_resident, 10);
    if (after_resident == after_total) {
        return 0;
    }
    const long page_size = ::sysconf(_SC_PAGESIZE);
    return page_size > 0 ? static_cast<u64>(resident_pages) * static_cast<u64>(page_size) : 0;
}

}  // namespace

Expected<usize, Error> executable_path(char* buffer, usize capacity) {
    char path[4096];
    const ssize_t length = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length <= 0) {
        return fail(ErrorCode::Unavailable, "/proc/self/exe could not be read");
    }
    path[length] = '\0';
    return write_to_buffer(buffer, capacity, std::string_view{path, static_cast<usize>(length)});
}

Status refine_memory_statistics(MemoryStatistics& statistics) {
    statistics.available_physical_bytes = read_meminfo_bytes("MemAvailable");
    statistics.process_resident_bytes = read_resident_bytes();
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

    // X11 first: its window is a number, not a pointer, which is exactly the sort of difference a
    // renderer must never have to know about.
    const Sint64 x11_window =
        SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    if (x11_window != 0) {
        // X11 names a window by number, and NativeSurface::handle is the one opaque field every
        // backend's handle passes through; it is carried, never dereferenced.
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        surface.handle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(x11_window));
        surface.display =
            SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        return true;
    }

    void* wayland_surface =
        SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    if (wayland_surface != nullptr) {
        surface.handle = wayland_surface;
        surface.display =
            SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        return true;
    }

    // Neither: the dummy or offscreen video driver, which has no native window to hand out.
    return false;
}

}  // namespace cy::host
