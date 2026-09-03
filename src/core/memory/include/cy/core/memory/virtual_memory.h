#pragma once
// Virtual address reservation, and the arena that commits into one. Task 2.9.
//
// `core-memory-and-containers` — "Virtual address reservation": where the platform supports it,
// large caches and stable arenas MAY reserve a virtual address range and commit pages on demand, so
// an arena can grow without relocating and without committing its maximum size. Reservation is a
// DECLARED PROPERTY of an allocator, not a global default, and its use is justified by measurement.
// Reserved-but-uncommitted address space is excluded from budgets and reported separately, since it
// is not memory in use.
//
// PLATFORM CONDITIONALS, AND WHY THESE ONES. design.md §9 asks for no new platform-conditional code
// without a stated reason. The reason here is that reserve-and-commit has no portable spelling:
// POSIX is `mmap(PROT_NONE)` then `mprotect`, Windows is `VirtualAlloc(MEM_RESERVE)` then
// `MEM_COMMIT`. They are two translation units selected by CMake, the way `src/core/diagnostics/`
// splits its crash handlers, rather than an #ifdef inside a shared file. The Windows unit has never
// been compiled — Linux is the only platform this milestone could test on.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>

namespace cy {

struct VirtualMemoryInfo {
    /// The commit granularity. Every offset and length passed to commit/decommit is rounded to it.
    usize page_size = 4096;
    /// The granularity a reservation's base address is aligned to. Equal to the page size on POSIX;
    /// 64 KiB on Windows, which is why it is a separate figure rather than an assumption.
    usize allocation_granularity = 4096;
    bool supported = false;
};

[[nodiscard]] VirtualMemoryInfo virtual_memory_info() noexcept;

/// Reserve `bytes` of address space without committing any of it. Null on failure.
[[nodiscard]] void* virtual_reserve(usize bytes) noexcept;

/// Make `[offset, offset + bytes)` of a reservation readable and writable. Idempotent.
[[nodiscard]] bool virtual_commit(void* base, usize offset, usize bytes) noexcept;

/// Give the pages back to the operating system while keeping the address range reserved.
[[nodiscard]] bool virtual_decommit(void* base, usize offset, usize bytes) noexcept;

/// Release the whole reservation.
void virtual_release(void* base, usize bytes) noexcept;

/// A bump arena over a reservation: it commits as it grows, so a cache can grow without moving and
/// existing pointers stay valid.
///
/// `reserves_address_space()` answers true, which is how a report knows to count this allocator's
/// committed bytes against a budget and its reserved bytes separately.
class VirtualArena final : public Allocator {
public:
    VirtualArena(MemoryDomain domain, AllocationTag tag) noexcept : Allocator(domain, tag) {}
    ~VirtualArena() override;

    /// Reserve the address range. Nothing is committed yet, and nothing counts against a budget.
    [[nodiscard]] Status reserve(usize bytes) noexcept;

    /// THE HOT PATH once the pages are committed: a bump. It leaves the fast path only when the
    /// allocation crosses into pages that have not been committed yet.
    [[nodiscard]] void* bump(usize size, usize alignment = kDefaultAlignment) noexcept;

    /// Wind back to empty, keeping the commitment. The pages stay resident, which is what makes a
    /// per-frame virtual arena as cheap as an ordinary one after the first frame.
    void reset() noexcept;

    /// Wind back to empty and give the pages back. The `Critical` pressure response.
    void decommit_all() noexcept;

    [[nodiscard]] bool reserves_address_space() const noexcept override { return true; }

    [[nodiscard]] usize reserved_bytes() const noexcept { return reserved_; }
    /// The figure that counts against a budget. See the note at the top of this file.
    [[nodiscard]] usize committed_bytes() const noexcept { return committed_; }
    [[nodiscard]] usize used() const noexcept { return offset_; }
    [[nodiscard]] usize high_water() const noexcept { return high_water_; }
    [[nodiscard]] u64 commit_failures() const noexcept { return commit_failures_; }

protected:
    [[nodiscard]] void* do_allocate(usize size, usize alignment) noexcept override;
    [[nodiscard]] void* do_reallocate(void* pointer, usize old_size, usize new_size,
                                      usize alignment) noexcept override;
    void do_deallocate(void* pointer, usize size, usize alignment) noexcept override;

private:
    /// Commit up to and including `wanted` bytes from the base, rounded up to whole pages.
    [[nodiscard]] bool commit_through(usize wanted) noexcept;

    u8* base_ = nullptr;
    usize reserved_ = 0;
    usize committed_ = 0;
    usize offset_ = 0;
    usize high_water_ = 0;
    usize page_size_ = 4096;
    u64 commit_failures_ = 0;
};

}  // namespace cy
