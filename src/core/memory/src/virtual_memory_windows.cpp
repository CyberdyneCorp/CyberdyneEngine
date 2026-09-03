// Reserve and commit on Windows. Task 2.9.
//
// UNVERIFIED. Windows has never been compiled in this repository — design.md §9 records that four
// M0 files are in the same position — so this file is the Windows spelling of the POSIX unit beside
// it, written from the documented contract of VirtualAlloc and not from a build. CI on Windows is
// what turns it from prose into code, and until then a reader should treat it as a claim.
//
// The differences from POSIX that this file exists for:
//   * reservation and commitment are the same call with different flags, not mmap then mprotect;
//   * a reservation's base is aligned to the allocation granularity (64 KiB), not to a page;
//   * decommitting is MEM_DECOMMIT, which keeps the reservation, and releasing takes a size of 0.

#include <cy/core/memory/virtual_memory.h>

#if defined(_WIN32)

#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>

namespace cy {
namespace {

[[nodiscard]] const SYSTEM_INFO& system_info() noexcept {
    static const SYSTEM_INFO info = [] {
        SYSTEM_INFO value{};
        ::GetSystemInfo(&value);
        return value;
    }();
    return info;
}

[[nodiscard]] usize round_to_page(usize bytes) noexcept {
    const usize page = static_cast<usize>(system_info().dwPageSize);
    return ((bytes + page - 1) / page) * page;
}

}  // namespace

VirtualMemoryInfo virtual_memory_info() noexcept {
    VirtualMemoryInfo info;
    info.page_size = static_cast<usize>(system_info().dwPageSize);
    info.allocation_granularity = static_cast<usize>(system_info().dwAllocationGranularity);
    info.supported = true;
    return info;
}

void* virtual_reserve(usize bytes) noexcept {
    if (bytes == 0) {
        return nullptr;
    }
    return ::VirtualAlloc(nullptr, round_to_page(bytes), MEM_RESERVE, PAGE_NOACCESS);
}

bool virtual_commit(void* base, usize offset, usize bytes) noexcept {
    if (base == nullptr || bytes == 0) {
        return false;
    }
    auto* address = static_cast<u8*>(base) + offset;
    return ::VirtualAlloc(address, round_to_page(bytes), MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

bool virtual_decommit(void* base, usize offset, usize bytes) noexcept {
    if (base == nullptr || bytes == 0) {
        return false;
    }
    auto* address = static_cast<u8*>(base) + offset;
    return ::VirtualFree(address, round_to_page(bytes), MEM_DECOMMIT) != 0;
}

void virtual_release(void* base, usize bytes) noexcept {
    (void)bytes;  // MEM_RELEASE requires a size of zero and releases the whole reservation
    if (base != nullptr) {
        (void)::VirtualFree(base, 0, MEM_RELEASE);
    }
}

}  // namespace cy

#endif  // _WIN32
