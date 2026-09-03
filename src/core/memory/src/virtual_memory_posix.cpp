// Reserve and commit on POSIX. Task 2.9.
//
// The idiom is one reservation with PROT_NONE — which maps address space without backing it — and
// mprotect(PROT_READ | PROT_WRITE) to commit. Linux over-commits by default, so a reservation
// costs a virtual mapping and nothing else; a page becomes real when it is first written.
//
// Selected by CMake for every non-Windows platform. macOS uses the same calls and has not been
// compiled here; see the note in virtual_memory.h.

#include <cy/core/memory/virtual_memory.h>

#include <sys/mman.h>
#include <unistd.h>

namespace cy {
namespace {

[[nodiscard]] usize page_size() noexcept {
    static const usize size = static_cast<usize>(sysconf(_SC_PAGESIZE));
    return (size == 0) ? 4096 : size;
}

[[nodiscard]] usize round_to_page(usize bytes) noexcept {
    const usize page = page_size();
    return ((bytes + page - 1) / page) * page;
}

}  // namespace

VirtualMemoryInfo virtual_memory_info() noexcept {
    VirtualMemoryInfo info;
    info.page_size = page_size();
    info.allocation_granularity = page_size();
    info.supported = true;
    return info;
}

void* virtual_reserve(usize bytes) noexcept {
    if (bytes == 0) {
        return nullptr;
    }
    void* mapping = ::mmap(nullptr, round_to_page(bytes), PROT_NONE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return (mapping == MAP_FAILED) ? nullptr : mapping;
}

bool virtual_commit(void* base, usize offset, usize bytes) noexcept {
    if (base == nullptr || bytes == 0) {
        return false;
    }
    auto* address = static_cast<u8*>(base) + offset;
    return ::mprotect(address, round_to_page(bytes), PROT_READ | PROT_WRITE) == 0;
}

bool virtual_decommit(void* base, usize offset, usize bytes) noexcept {
    if (base == nullptr || bytes == 0) {
        return false;
    }
    auto* address = static_cast<u8*>(base) + offset;
    const usize length = round_to_page(bytes);
    // MADV_DONTNEED returns the pages to the kernel; the mapping stays, so the addresses remain
    // reserved and a later commit does not have to find them again. PROT_NONE afterwards is what
    // makes a stale read fault rather than silently reading zeroes.
    (void)::madvise(address, length, MADV_DONTNEED);
    return ::mprotect(address, length, PROT_NONE) == 0;
}

void virtual_release(void* base, usize bytes) noexcept {
    if (base != nullptr && bytes != 0) {
        (void)::munmap(base, round_to_page(bytes));
    }
}

}  // namespace cy
