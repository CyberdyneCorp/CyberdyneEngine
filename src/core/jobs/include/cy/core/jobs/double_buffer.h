#pragma once
// Double buffering across thread boundaries, and the lock-free single-producer queue. Task 3.2.9.
//
// `core-jobs-and-concurrency`: "State read by one thread and written by another SHALL be double
// buffered where possible, with an explicit swap at a defined synchronisation point, rather than
// shared under a lock. This SHALL apply to: transforms consumed by rendering, event channels, input
// state, and debug-draw command lists."
//
// The shape is deliberately not a lock with a nicer name. The reader reads the *published* buffer
// and the writer writes the *pending* one; neither ever touches the other's, so there is nothing to
// synchronise between them and nothing for the reader to wait on. The only ordering is the swap,
// which happens at a defined point with nobody reading — that is the whole contract, and it is what
// makes "the render thread reads the previous frame's snapshot while simulation writes the next"
// true rather than aspirational.
//
// SpscQueue is the other mechanism the specification names: a bounded lock-free queue with exactly
// one producer thread and one consumer thread. It is what the asset I/O thread publishes through,
// because "it SHALL publish the result through a queue consumed on the simulation thread" is a
// queue, not a mutex around a vector.

#include <cy/core/jobs/sync.h>
#include <cy/core/jobs/types.h>

#include <atomic>
#include <new>
#include <utility>

namespace cy::jobs {

/// Two instances of T: one published, one being written.
///
/// `read()` is the published one and is safe from any thread between swaps. `write()` is the
/// pending one and belongs to the writer alone. `swap()` publishes, and must be called at a
/// synchronisation point where nobody is reading — the frame's snapshot-publication point.
template <class T>
class DoubleBuffered {
public:
    DoubleBuffered() = default;

    explicit DoubleBuffered(const T& initial) : buffers_{initial, initial} {}

    DoubleBuffered(const DoubleBuffered&) = delete;
    DoubleBuffered& operator=(const DoubleBuffered&) = delete;

    /// The published buffer. What a reader on another thread sees.
    [[nodiscard]] const T& read() const noexcept {
        return buffers_[published_.load(std::memory_order_acquire)];
    }

    /// The buffer being prepared. The writer's alone until the next swap.
    [[nodiscard]] T& write() noexcept {
        return buffers_[1 - published_.load(std::memory_order_relaxed)];
    }

    /// Publish what was written, and hand the writer what the readers were reading.
    ///
    /// Release, so that everything the writer stored into the pending buffer is visible to a reader
    /// that acquires the new index. This is the one ordering point in the whole mechanism.
    void swap() noexcept {
        const u32 current = published_.load(std::memory_order_relaxed);
        published_.store(1 - current, std::memory_order_release);
        ++generation_;
    }

    /// How many swaps have happened. A reader that wants to know whether it is looking at new data
    /// compares this rather than the contents.
    [[nodiscard]] u64 generation() const noexcept { return generation_; }

private:
    T buffers_[2] = {};
    std::atomic<u32> published_{0};
    u64 generation_ = 0;
};

/// A bounded lock-free queue with exactly one producer thread and one consumer thread.
///
/// One producer and one consumer is not a limitation to work around; it is the reason the queue
/// needs no compare-and-swap at all. Two producers would need one, and at that point the right
/// answer is usually to give each producer its own queue.
template <class T>
class SpscQueue {
public:
    static_assert(std::is_trivially_copyable_v<T>,
                  "an SPSC queue slot is written by one thread and read by another with no lock; a "
                  "type with a non-trivial copy needs a different mechanism");

    SpscQueue() noexcept = default;
    ~SpscQueue() {
        delete[] slots_;
        slots_ = nullptr;
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    /// Reserve `capacity` slots. One allocation, made before either thread starts.
    Status initialize(u32 capacity) noexcept {
        if (capacity < 2) {
            return fail(ErrorCode::InvalidArgument,
                        "an SPSC queue needs at least two slots: one is always left empty so that "
                        "full and empty are distinguishable without a third counter");
        }
        delete[] slots_;
        slots_ = new (std::nothrow) T[capacity];
        if (slots_ == nullptr) {
            capacity_ = 0;
            return fail(ErrorCode::OutOfMemory, "the SPSC queue could not be allocated");
        }
        capacity_ = capacity;
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        return ok();
    }

    /// The producer's call. False when the queue is full — which is a result, not an assertion: a
    /// full queue means the consumer is behind, and the producer decides what to do about that.
    [[nodiscard]] bool push(const T& value) noexcept {
        const u32 tail = tail_.load(std::memory_order_relaxed);
        const u32 next = tail + 1 == capacity_ ? 0 : tail + 1;
        if (next == head_.load(std::memory_order_acquire)) {
            return false;
        }
        slots_[tail] = value;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    /// The consumer's call. False when the queue is empty.
    [[nodiscard]] bool pop(T& out) noexcept {
        const u32 head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        out = slots_[head];
        head_.store(head + 1 == capacity_ ? 0 : head + 1, std::memory_order_release);
        return true;
    }

    /// An approximation from either side: exact for the thread that owns the end it reads.
    [[nodiscard]] u32 size() const noexcept {
        const u32 tail = tail_.load(std::memory_order_acquire);
        const u32 head = head_.load(std::memory_order_acquire);
        return tail >= head ? tail - head : capacity_ - head + tail;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] u32 capacity() const noexcept { return capacity_; }

private:
    T* slots_ = nullptr;
    u32 capacity_ = 0;
    // On their own cache lines: a producer's store to `tail_` invalidating the line the consumer is
    // reading `head_` from is the whole cost of an SPSC queue that has been written carelessly.
    alignas(64) std::atomic<u32> head_{0};
    alignas(64) std::atomic<u32> tail_{0};
};

}  // namespace cy::jobs
