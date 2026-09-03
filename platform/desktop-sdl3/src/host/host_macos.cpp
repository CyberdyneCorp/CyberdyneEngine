// macOS host services. See host.h; CMakeLists.txt compiles this file only on macOS.
//
// UNVERIFIED. Written against the documented Darwin surface and reviewed, but M0 was developed on
// Linux and no macOS host was available to run it. The first macOS CI job (task 2.4.1) is what
// turns this comment into a fact.

#include "host.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdlib>

#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <unistd.h>

namespace cy::host {

Expected<usize, Error> executable_path(char* buffer, usize capacity) {
    char raw[4096];
    std::uint32_t size = sizeof(raw);
    if (_NSGetExecutablePath(raw, &size) != 0) {
        return fail(ErrorCode::BufferTooSmall, "the executable path is longer than 4096 bytes");
    }
    // _NSGetExecutablePath may return a path containing symlinks or "..", so it is resolved before
    // it is handed out; a caller comparing two paths would otherwise get the wrong answer.
    char resolved[4096];
    const char* result = ::realpath(raw, resolved);
    return write_to_buffer(buffer, capacity, result != nullptr ? result : raw);
}

Status refine_memory_statistics(MemoryStatistics& statistics) {
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm),
                          &count) == KERN_SUCCESS) {
        const u64 page_size = static_cast<u64>(::getpagesize());
        // "Available" on Darwin is free plus what the kernel would reclaim on demand; counting only
        // free_count would report a machine with a warm file cache as nearly out of memory.
        const u64 pages = static_cast<u64>(vm.free_count) + static_cast<u64>(vm.inactive_count) +
                          static_cast<u64>(vm.purgeable_count);
        statistics.available_physical_bytes = pages * page_size;
    }

    mach_task_basic_info_data_t task_information{};
    mach_msg_type_number_t task_count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&task_information), &task_count) == KERN_SUCCESS) {
        statistics.process_resident_bytes = static_cast<u64>(task_information.resident_size);
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
    void* cocoa_window =
        SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (cocoa_window == nullptr) {
        return false;
    }
    // The NSWindow. A Metal backend at M3 asks for GraphicsApi::Metal and gets a CAMetalLayer
    // instead; this is the M0 seam, and it is the object that layer would be attached to.
    surface.handle = cocoa_window;
    surface.display = nullptr;
    return true;
}

}  // namespace cy::host
