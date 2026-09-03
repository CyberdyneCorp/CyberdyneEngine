#pragma once
// Per-task scratch memory. Task 3.2.3.
//
// `core-jobs-and-concurrency`: temporary allocation within a task defaults to the task's scratch
// allocator, so the common case is contention-free and reclaimed in bulk without the author
// choosing an allocator. Scratch does not outlive the task, and development builds poison it on
// release.
//
// A scratch arena is a bump pointer over one block owned by one worker. There is no free: the job
// system takes a mark before a task's body and releases to it afterwards, which reclaims everything
// the task allocated in a single store. Nothing in the arena is destructed, so what goes in it is
// trivially destructible; that is enforced by a static_assert rather than by a convention.
//
// WHY THE POISON IS NOT DEBUG-ONLY DECORATION. A pointer into scratch that outlives its task reads
// as valid memory belonging to the next task, and the bug is a value that is subtly wrong rather
// than a crash. Overwriting on release turns it into an obvious one, in the configurations that can
// afford it: Debug and Development poison, Profile and Shipping do not.
//
// The backing block is one allocation per worker, made when the job system starts. Task 2.1's
// allocator interface plugs in at `ScratchArena::initialize`, which is the only place this file
// touches the general heap.

#include <cy/core/base/assert.h>
#include <cy/core/jobs/types.h>

#include <type_traits>

namespace cy::jobs {

/// The byte written over released scratch in a development build. Not zero, and not a plausible
/// pointer or float: a value that shows up in a debugger as obviously not data.
inline constexpr u8 kScratchPoison = 0xCD;

class ScratchArena {
public:
    ScratchArena() noexcept = default;
    ~ScratchArena();

    ScratchArena(const ScratchArena&) = delete;
    ScratchArena& operator=(const ScratchArena&) = delete;
    ScratchArena(ScratchArena&& other) noexcept;
    ScratchArena& operator=(ScratchArena&& other) noexcept;

    /// Reserve the block. Called once, when the job system starts.
    Status initialize(usize bytes) noexcept;

    /// Bytes from the arena, aligned. Null when the arena is exhausted — which is a *result*, not
    /// an assertion: a task that wants more scratch than a worker has should say so and fall back,
    /// and the arena counts the refusal so that the fallback is visible rather than silent.
    [[nodiscard]] void* allocate(usize bytes, usize alignment) noexcept;

    /// The common spelling. `T` must be trivially destructible: the arena never destructs.
    template <class T>
    [[nodiscard]] T* allocate_array(usize count) noexcept {
        static_assert(std::is_trivially_destructible_v<T>,
                      "scratch is reclaimed in bulk and never destructs; a type with a destructor "
                      "belongs in a container over the allocator interface, not in scratch");
        if (count == 0) {
            return nullptr;
        }
        if (count > (static_cast<usize>(-1) / sizeof(T))) {
            return nullptr;
        }
        return static_cast<T*>(allocate(count * sizeof(T), alignof(T)));
    }

    /// The current offset. Paired with `release_to`.
    [[nodiscard]] usize mark() const noexcept { return used_; }

    /// Reclaim everything allocated since `mark`, poisoning it in a development build.
    void release_to(usize mark) noexcept;

    /// Reclaim everything.
    void reset() noexcept { release_to(0); }

    [[nodiscard]] usize used() const noexcept { return used_; }
    [[nodiscard]] usize capacity() const noexcept { return capacity_; }
    /// The most that was ever in use. What a per-worker scratch budget is set from.
    [[nodiscard]] usize high_water() const noexcept { return high_water_; }
    /// How many allocations this arena refused because it was full.
    [[nodiscard]] u64 exhaustions() const noexcept { return exhaustions_; }

private:
    u8* block_ = nullptr;
    usize capacity_ = 0;
    usize used_ = 0;
    usize high_water_ = 0;
    u64 exhaustions_ = 0;
};

/// Take a mark on construction, release to it on destruction. The job system wraps every task body
/// in one; a caller that wants a nested region inside a long task uses it directly.
class ScratchScope {
public:
    explicit ScratchScope(ScratchArena& arena) noexcept : arena_(arena), mark_(arena.mark()) {}
    ~ScratchScope() { arena_.release_to(mark_); }

    ScratchScope(const ScratchScope&) = delete;
    ScratchScope& operator=(const ScratchScope&) = delete;

    [[nodiscard]] ScratchArena& arena() const noexcept { return arena_; }

private:
    ScratchArena& arena_;
    usize mark_;
};

}  // namespace cy::jobs
