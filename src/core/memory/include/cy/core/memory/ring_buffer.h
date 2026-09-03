#pragma once
// A bounded FIFO. Task 2.4.
//
// `core-memory-and-containers` — "Sequence containers": `RingBuffer<T>` is the bounded FIFO for
// audio and networking. Bounded is the property that matters for both: an audio ring that grew
// would allocate on the audio thread, and a network ring that grew would let a peer decide how much
// memory the process uses.
//
// The capacity is a power of two so the index arithmetic is a mask rather than a modulo, and the
// buffer is allocated once at construction. `push` fails when it is full; it does not overwrite,
// because a caller that wants the newest and can drop the oldest says so with `force_push`.
//
// SINGLE-THREADED. This is a container, not a queue between threads: the lock-free single-producer
// single-consumer version belongs to `core-jobs-and-concurrency`, where the memory ordering can be
// stated against the thread roles that exist there.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/relocatable.h>
#include <cy/core/memory/scope.h>

#include <utility>

namespace cy {

template <class T>
class RingBuffer {
public:
    explicit RingBuffer(Allocator& allocator = current_allocator()) noexcept
        : allocator_(&allocator) {}

    ~RingBuffer() { release(); }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    RingBuffer(RingBuffer&& other) noexcept
        : allocator_(other.allocator_),
          data_(other.data_),
          mask_(other.mask_),
          head_(other.head_),
          tail_(other.tail_) {
        other.data_ = nullptr;
        other.mask_ = 0;
        other.head_ = 0;
        other.tail_ = 0;
    }

    RingBuffer& operator=(RingBuffer&& other) noexcept {
        if (this != &other) {
            release();
            allocator_ = other.allocator_;
            data_ = other.data_;
            mask_ = other.mask_;
            head_ = other.head_;
            tail_ = other.tail_;
            other.data_ = nullptr;
            other.mask_ = 0;
            other.head_ = 0;
            other.tail_ = 0;
        }
        return *this;
    }

    /// Allocate the ring. `capacity` is rounded up to a power of two, and the rounded figure is
    /// what `capacity()` reports — a caller that asked for 100 and got 128 should be told so.
    [[nodiscard]] Status reserve(usize capacity) noexcept {
        release();
        usize rounded = 1;
        while (rounded < capacity) {
            rounded *= 2;
        }
        void* block = allocator_->allocate(rounded * sizeof(T), alignof(T));
        if (block == nullptr) {
            return fail(ErrorCode::OutOfMemory, "RingBuffer reservation was refused");
        }
        data_ = static_cast<T*>(block);
        mask_ = rounded - 1;
        return ok();
    }

    template <class... Args>
    [[nodiscard]] Expected<T*, Error> emplace(Args&&... args) noexcept {
        if (data_ == nullptr) {
            return fail(ErrorCode::Unavailable, "RingBuffer has no capacity; call reserve() first");
        }
        if (full()) {
            return fail(ErrorCode::OutOfRange, "RingBuffer is full");
        }
        return construct_at<T>(data_ + (tail_++ & mask_), std::forward<Args>(args)...);
    }

    [[nodiscard]] Status push(const T& value) noexcept {
        Expected<T*, Error> slot = emplace(value);
        return slot ? ok() : Status{make_unexpected(slot.error())};
    }

    /// Push, discarding the oldest element when the ring is full. Returns whether something was
    /// dropped, so the caller can count it — a dropped audio block or packet that nothing counted
    /// is the silent loss this engine treats as a defect.
    [[nodiscard]] bool force_push(const T& value) noexcept {
        bool dropped = false;
        if (full()) {
            pop();
            dropped = true;
        }
        (void)push(value);
        return dropped;
    }

    /// The oldest element, or null when empty. A pointer rather than an Expected: `front()` is
    /// called in a loop until it answers null, and that reads better than a status per iteration.
    [[nodiscard]] T* front() noexcept { return empty() ? nullptr : data_ + (head_ & mask_); }
    [[nodiscard]] const T* front() const noexcept {
        return empty() ? nullptr : data_ + (head_ & mask_);
    }

    void pop() noexcept {
        if (!empty()) {
            data_[head_++ & mask_].~T();
        }
    }

    void clear() noexcept {
        while (!empty()) {
            pop();
        }
    }

    [[nodiscard]] usize size() const noexcept { return tail_ - head_; }
    [[nodiscard]] usize capacity() const noexcept { return (data_ == nullptr) ? 0 : mask_ + 1; }
    [[nodiscard]] bool empty() const noexcept { return head_ == tail_; }
    [[nodiscard]] bool full() const noexcept { return size() == capacity(); }

private:
    void release() noexcept {
        if (data_ == nullptr) {
            return;
        }
        clear();
        allocator_->deallocate(data_, (mask_ + 1) * sizeof(T), alignof(T));
        data_ = nullptr;
        mask_ = 0;
        head_ = 0;
        tail_ = 0;
    }

    Allocator* allocator_;
    T* data_ = nullptr;
    usize mask_ = 0;
    // Free-running counters rather than wrapped indices: the difference between them is the size,
    // with no ambiguity between "full" and "empty" and no spare slot given up to disambiguate them.
    usize head_ = 0;
    usize tail_ = 0;
};

}  // namespace cy
