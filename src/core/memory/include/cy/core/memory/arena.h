#pragma once
// Bump allocation: the arena, the LIFO stack over it, and the poison that catches a stale read.
// Tasks 2.1 and 2.7.
//
// `core-memory-and-containers` — "Per-frame scratch is freed in O(1)": a frame ends by resetting an
// offset, without running destructors for trivially destructible data. And "Use after reset is
// caught": development builds fill a reset region with a poison pattern so a stale read is obvious
// rather than plausible.
//
// THE ACCOUNTING BOUNDARY. An arena charges its domain once, when it takes its block from the
// upstream allocator, and never again. Ten thousand bump allocations inside a frame cost nothing to
// account for, and the domain report says what the frame arena actually costs the process — its
// reservation — rather than the sum of temporaries that were never separately resident.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>

namespace cy {

/// The byte written over reset arena memory in development builds. Chosen to be a value that is
/// neither a plausible pointer nor a plausible small integer: 0xDD repeated is 15,987,178,197,214,
/// which a debugger prints in a way that says "this is poison" at a glance.
inline constexpr u8 kArenaPoisonByte = 0xDD;

namespace detail {

/// A block of bytes and an offset into it. Shared by the arena and the stack, which differ in what
/// they let a caller do with the offset, not in how they move it.
///
/// This is deliberately not an allocator: it has no domain, no tag and no virtual table, so a
/// header that only needs the arithmetic does not acquire an interface.
class BumpRegion {
public:
    [[nodiscard]] void* bump(usize size, usize alignment) noexcept {
        const usize aligned = align_up(offset_, alignment);
        if (aligned + size > capacity_ || data_ == nullptr) {
            ++failures_;
            return nullptr;
        }
        offset_ = aligned + size;
        if (offset_ > high_water_) {
            high_water_ = offset_;
        }
        return data_ + aligned;
    }

    void reset() noexcept {
        poison(0, offset_);
        offset_ = 0;
    }

    /// Wind the offset back to `marker`, poisoning what is given up. The caller has established
    /// that `marker` is not ahead of the current offset.
    void rewind(usize marker) noexcept {
        if (marker < offset_) {
            poison(marker, offset_ - marker);
            offset_ = marker;
        }
    }

    void adopt(u8* data, usize capacity) noexcept {
        data_ = data;
        capacity_ = capacity;
        offset_ = 0;
        high_water_ = 0;
    }

    [[nodiscard]] u8* data() const noexcept { return data_; }
    [[nodiscard]] usize capacity() const noexcept { return capacity_; }
    [[nodiscard]] usize used() const noexcept { return offset_; }
    [[nodiscard]] usize remaining() const noexcept { return capacity_ - offset_; }
    [[nodiscard]] usize high_water() const noexcept { return high_water_; }
    [[nodiscard]] u64 failures() const noexcept { return failures_; }
    [[nodiscard]] bool owns(const void* pointer) const noexcept {
        const auto* byte = static_cast<const u8*>(pointer);
        return data_ != nullptr && byte >= data_ && byte < data_ + capacity_;
    }

    /// Fill a range with the poison byte, in development builds only. In Profile and Shipping this
    /// compiles to nothing: the pattern is a debugging aid, and writing a megabyte of it at every
    /// frame boundary is not something a shipping build should pay for.
    void poison(usize offset, usize bytes) noexcept;

private:
    u8* data_ = nullptr;
    usize capacity_ = 0;
    usize offset_ = 0;
    usize high_water_ = 0;
    u64 failures_ = 0;
};

}  // namespace detail

/// Bump allocation with a whole-region reset. Per-frame and per-load scratch.
///
/// `deallocate` is a no-op — that is what an arena is — except for the most recent allocation,
/// which is wound back so that a container that grows repeatedly inside one arena does not leave
/// every intermediate buffer behind it.
class ArenaAllocator final : public Allocator {
public:
    ArenaAllocator(MemoryDomain domain, AllocationTag tag) noexcept : Allocator(domain, tag) {}
    ~ArenaAllocator() override;

    /// Take `bytes` from `upstream` and own them. Replaces any block this arena already owns.
    Status reserve(usize bytes, Allocator& upstream) noexcept;
    Status reserve(usize bytes) noexcept;

    /// Use a caller-owned buffer. The arena does not free it, and the caller guarantees it outlives
    /// the arena. This is how a stack buffer or a sub-range of a larger block becomes an arena.
    void adopt(void* buffer, usize bytes) noexcept;

    /// THE HOT PATH. Non-virtual, inline, no dispatch: a caller with the concrete type in hand
    /// pays an add, a compare and a store. Null when the arena is full.
    [[nodiscard]] void* bump(usize size, usize alignment = kDefaultAlignment) noexcept {
        return region_.bump(size, alignment);
    }

    /// Free everything at once, in O(1). Destructors are not run: an arena is for data whose
    /// destruction is a no-op, and the caller that puts something else in one owns that decision.
    void reset() noexcept { region_.reset(); }

    using Marker = usize;
    [[nodiscard]] Marker mark() const noexcept { return region_.used(); }
    void rewind(Marker marker) noexcept { region_.rewind(marker); }

    [[nodiscard]] usize capacity() const noexcept { return region_.capacity(); }
    [[nodiscard]] usize used() const noexcept { return region_.used(); }
    [[nodiscard]] usize remaining() const noexcept { return region_.remaining(); }
    /// The most this arena has ever held at once — the figure that sizes it correctly next time.
    [[nodiscard]] usize high_water() const noexcept { return region_.high_water(); }
    /// How many bump requests this arena has refused. A frame arena with a non-zero count is
    /// undersized, and that is a report, not a crash.
    [[nodiscard]] u64 overflows() const noexcept { return region_.failures(); }
    [[nodiscard]] bool owns(const void* pointer) const noexcept { return region_.owns(pointer); }

protected:
    [[nodiscard]] void* do_allocate(usize size, usize alignment) noexcept override;
    [[nodiscard]] void* do_reallocate(void* pointer, usize old_size, usize new_size,
                                      usize alignment) noexcept override;
    void do_deallocate(void* pointer, usize size, usize alignment) noexcept override;

private:
    void release() noexcept;

    detail::BumpRegion region_;
    Allocator* upstream_ = nullptr;  // null when the block is borrowed rather than owned
};

/// LIFO allocation within a function or a job. The same arithmetic as the arena, with the release
/// order made explicit: a marker is taken, allocations happen, the marker is released, and anything
/// allocated after it is gone.
class StackAllocator final : public Allocator {
public:
    StackAllocator(MemoryDomain domain, AllocationTag tag) noexcept : Allocator(domain, tag) {}
    ~StackAllocator() override;

    Status reserve(usize bytes, Allocator& upstream) noexcept;
    Status reserve(usize bytes) noexcept;
    void adopt(void* buffer, usize bytes) noexcept;

    using Marker = usize;

    /// THE HOT PATH. Non-virtual and inline, like the arena's bump.
    [[nodiscard]] void* push(usize size, usize alignment = kDefaultAlignment) noexcept {
        return region_.bump(size, alignment);
    }

    [[nodiscard]] Marker mark() const noexcept { return region_.used(); }

    /// Release everything allocated since `marker`. Releasing a marker taken before another that
    /// is still outstanding is the LIFO violation this class exists to make visible; it trips an
    /// assertion in development and is otherwise honoured, because the alternative — silently
    /// keeping the memory — is the harder bug.
    void release(Marker marker) noexcept;

    void reset() noexcept { region_.reset(); }

    [[nodiscard]] usize capacity() const noexcept { return region_.capacity(); }
    [[nodiscard]] usize used() const noexcept { return region_.used(); }
    [[nodiscard]] usize high_water() const noexcept { return region_.high_water(); }
    [[nodiscard]] u64 overflows() const noexcept { return region_.failures(); }

protected:
    [[nodiscard]] void* do_allocate(usize size, usize alignment) noexcept override;
    [[nodiscard]] void* do_reallocate(void* pointer, usize old_size, usize new_size,
                                      usize alignment) noexcept override;
    void do_deallocate(void* pointer, usize size, usize alignment) noexcept override;

private:
    void release_block() noexcept;

    detail::BumpRegion region_;
    Allocator* upstream_ = nullptr;
};

/// A stack marker taken on construction and released on destruction. The shape a job's temporary
/// allocation should have, so that an early return cannot leak the scratch it took.
class StackScope {
public:
    explicit StackScope(StackAllocator& stack) noexcept : stack_(&stack), marker_(stack.mark()) {}
    ~StackScope() { stack_->release(marker_); }

    StackScope(const StackScope&) = delete;
    StackScope& operator=(const StackScope&) = delete;
    StackScope(StackScope&&) = delete;
    StackScope& operator=(StackScope&&) = delete;

    [[nodiscard]] StackAllocator& allocator() const noexcept { return *stack_; }

private:
    StackAllocator* stack_;
    StackAllocator::Marker marker_;
};

}  // namespace cy
