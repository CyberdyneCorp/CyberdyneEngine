#pragma once
// `core/threading` — the synchronisation vocabulary. Task 3.2.9.
//
// `core-jobs-and-concurrency` names exactly this set: Thread, Mutex, RecursiveMutex, RwLock,
// Semaphore, ConditionVariable, SpinLock, Event, and the atomic wrappers Atomic<T>, AtomicFlag and
// AtomicRefCount. They are thin over the standard library, and that is the point rather than a
// shortcut: the engine needs one spelling for each so that a later platform backend — a futex, a
// SRWLOCK, a parking lot — replaces the body of one class instead of every call site.
//
// The header carries the rule as well as the primitives. `core-jobs-and-concurrency` states that
// locks are avoided in per-entity hot paths and that the preferred mechanisms are the job system's
// dependency graph, double buffering and single-producer/single-consumer queues. A lock taken
// inside a per-entity loop is a design error, not a slow path: restructure into per-thread
// accumulation merged at the flush point. cy::jobs::DoubleBuffered (double_buffer.h) and
// cy::jobs::SpscQueue (both in double_buffer.h) are what that restructuring uses.

#include <cy/core/jobs/types.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <utility>

namespace cy::jobs {

// --- Atomics
// --------------------------------------------------------------------------------------
//
// Named wrappers rather than aliases, so that the engine's atomics can grow an operation the
// standard library spells differently — or none at all — without a rename across the tree. The
// default memory order is `seq_cst`, matching the standard library: a caller that has reasoned
// about a weaker order passes it explicitly, and a caller that has not gets the order that is
// hardest to be wrong with.

template <class T>
class Atomic {
public:
    Atomic() noexcept = default;
    explicit Atomic(T initial) noexcept : value_(initial) {}

    Atomic(const Atomic&) = delete;
    Atomic& operator=(const Atomic&) = delete;

    [[nodiscard]] T load(std::memory_order order = std::memory_order_seq_cst) const noexcept {
        return value_.load(order);
    }
    void store(T desired, std::memory_order order = std::memory_order_seq_cst) noexcept {
        value_.store(desired, order);
    }
    T exchange(T desired, std::memory_order order = std::memory_order_seq_cst) noexcept {
        return value_.exchange(desired, order);
    }
    bool compare_exchange_weak(T& expected, T desired,
                               std::memory_order order = std::memory_order_seq_cst) noexcept {
        return value_.compare_exchange_weak(expected, desired, order);
    }
    bool compare_exchange_strong(T& expected, T desired,
                                 std::memory_order order = std::memory_order_seq_cst) noexcept {
        return value_.compare_exchange_strong(expected, desired, order);
    }
    T fetch_add(T operand, std::memory_order order = std::memory_order_seq_cst) noexcept {
        return value_.fetch_add(operand, order);
    }
    T fetch_sub(T operand, std::memory_order order = std::memory_order_seq_cst) noexcept {
        return value_.fetch_sub(operand, order);
    }

private:
    std::atomic<T> value_{};
};

class AtomicFlag {
public:
    AtomicFlag() noexcept = default;
    explicit AtomicFlag(bool initial) noexcept : value_(initial) {}

    AtomicFlag(const AtomicFlag&) = delete;
    AtomicFlag& operator=(const AtomicFlag&) = delete;

    [[nodiscard]] bool test(std::memory_order order = std::memory_order_seq_cst) const noexcept {
        return value_.load(order);
    }
    /// Sets the flag and returns what it was, so `if (!flag.test_and_set()) { … }` is the
    /// once-only idiom without a second variable.
    bool test_and_set(std::memory_order order = std::memory_order_seq_cst) noexcept {
        return value_.exchange(true, order);
    }
    void clear(std::memory_order order = std::memory_order_seq_cst) noexcept {
        value_.store(false, order);
    }

private:
    std::atomic<bool> value_{false};
};

/// A reference count. `add_ref` returns the new count and `release` returns true when the count
/// reached zero and the caller therefore owns the destruction.
///
/// The decrement is `acq_rel` rather than the textbook `release` followed by a standalone acquire
/// fence on the last one. The two are equivalent for the reader — everything every earlier thread
/// wrote is visible to whoever destroys the object either way — but a standalone
/// `atomic_thread_fence` is invisible to ThreadSanitizer, which models happens-before through
/// atomic operations and not through fences. GCC refuses to compile one under `-fsanitize=thread`
/// for exactly that reason, and it is right to: a fence here would make every race this class is
/// meant to prevent undetectable in the one build that looks for them. The cost is an acquire on
/// each decrement rather than only the last, which is nothing measurable against a shared
/// cache line.
class AtomicRefCount {
public:
    AtomicRefCount() noexcept = default;
    explicit AtomicRefCount(u32 initial) noexcept : count_(initial) {}

    AtomicRefCount(const AtomicRefCount&) = delete;
    AtomicRefCount& operator=(const AtomicRefCount&) = delete;

    u32 add_ref() noexcept { return count_.fetch_add(1, std::memory_order_relaxed) + 1; }

    [[nodiscard]] bool release() noexcept {
        return count_.fetch_sub(1, std::memory_order_acq_rel) == 1;
    }

    [[nodiscard]] u32 count() const noexcept { return count_.load(std::memory_order_relaxed); }

private:
    std::atomic<u32> count_{0};
};

// --- Locks
// ----------------------------------------------------------------------------------------

class Mutex {
public:
    Mutex() noexcept = default;
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock() noexcept { mutex_.lock(); }
    void unlock() noexcept { mutex_.unlock(); }
    [[nodiscard]] bool try_lock() noexcept { return mutex_.try_lock(); }

    /// For the cases that need the standard library's own type — a condition variable's wait.
    [[nodiscard]] std::mutex& native() noexcept { return mutex_; }

private:
    std::mutex mutex_;
};

class RecursiveMutex {
public:
    RecursiveMutex() noexcept = default;
    RecursiveMutex(const RecursiveMutex&) = delete;
    RecursiveMutex& operator=(const RecursiveMutex&) = delete;

    void lock() noexcept { mutex_.lock(); }
    void unlock() noexcept { mutex_.unlock(); }
    [[nodiscard]] bool try_lock() noexcept { return mutex_.try_lock(); }

private:
    std::recursive_mutex mutex_;
};

/// Many readers or one writer. Named RwLock rather than SharedMutex because that is the name the
/// specification uses and the name every call site will reach for.
class RwLock {
public:
    RwLock() noexcept = default;
    RwLock(const RwLock&) = delete;
    RwLock& operator=(const RwLock&) = delete;

    void lock_shared() noexcept { mutex_.lock_shared(); }
    void unlock_shared() noexcept { mutex_.unlock_shared(); }
    [[nodiscard]] bool try_lock_shared() noexcept { return mutex_.try_lock_shared(); }

    void lock() noexcept { mutex_.lock(); }
    void unlock() noexcept { mutex_.unlock(); }
    [[nodiscard]] bool try_lock() noexcept { return mutex_.try_lock(); }

private:
    std::shared_mutex mutex_;
};

/// A test-and-test-and-set spin lock, for a critical section measured in tens of instructions.
///
/// It backs the job system's own deques, where the section is a handful of loads and stores and a
/// mutex would cost more in the uncontended case than the section itself. It is not a general
/// substitute for Mutex: a spin lock held across anything that can be descheduled burns a core.
class SpinLock {
public:
    SpinLock() noexcept = default;
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    void lock() noexcept {
        for (;;) {
            if (!locked_.exchange(true, std::memory_order_acquire)) {
                return;
            }
            // Read-only until it looks free: the exchange above is a write, and spinning on a write
            // keeps the cache line bouncing between every waiter.
            while (locked_.load(std::memory_order_relaxed)) {
                cpu_relax();
            }
        }
    }

    [[nodiscard]] bool try_lock() noexcept {
        return !locked_.exchange(true, std::memory_order_acquire);
    }

    void unlock() noexcept { locked_.store(false, std::memory_order_release); }

    /// The architecture's "I am spinning" hint. A no-op where there is none.
    static void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ __volatile__("yield" ::: "memory");
#else
        std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
    }

private:
    std::atomic<bool> locked_{false};
};

/// RAII over anything with lock()/unlock(). Named ScopedLock rather than reusing
/// std::lock_guard so that SpinLock and Mutex are held the same way at every call site.
template <class Lockable>
class ScopedLock {
public:
    explicit ScopedLock(Lockable& lockable) noexcept : lockable_(lockable) { lockable_.lock(); }
    ~ScopedLock() noexcept { lockable_.unlock(); }

    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;

private:
    Lockable& lockable_;
};

template <class Lockable>
class SharedLock {
public:
    explicit SharedLock(Lockable& lockable) noexcept : lockable_(lockable) {
        lockable_.lock_shared();
    }
    ~SharedLock() noexcept { lockable_.unlock_shared(); }

    SharedLock(const SharedLock&) = delete;
    SharedLock& operator=(const SharedLock&) = delete;

private:
    Lockable& lockable_;
};

// --- Waiting ----------------------------------------------------------------------------------

class ConditionVariable {
public:
    ConditionVariable() noexcept = default;
    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;

    void notify_one() noexcept { cv_.notify_one(); }
    void notify_all() noexcept { cv_.notify_all(); }

    /// Waits until `predicate()` holds. The predicate form is the only one offered: the bare wait
    /// is correct only when the caller has already written the loop the predicate form writes.
    template <class Predicate>
    void wait(Mutex& mutex, Predicate predicate) noexcept {
        std::unique_lock<std::mutex> lock(mutex.native(), std::adopt_lock);
        cv_.wait(lock, predicate);
        lock.release();
    }

    /// Returns false on timeout. `timeout_ns` is a duration, not a deadline.
    template <class Predicate>
    bool wait_for(Mutex& mutex, i64 timeout_ns, Predicate predicate) noexcept {
        std::unique_lock<std::mutex> lock(mutex.native(), std::adopt_lock);
        const bool satisfied = cv_.wait_for(lock, std::chrono::nanoseconds(timeout_ns), predicate);
        lock.release();
        return satisfied;
    }

private:
    std::condition_variable cv_;
};

/// A counting semaphore. std::counting_semaphore is C++20 and present in every supported toolchain,
/// but it has no way to report a failed acquire without a timeout, so this is written over a mutex
/// and a condition variable where the shape is explicit.
class Semaphore {
public:
    explicit Semaphore(u32 initial = 0) noexcept : count_(initial) {}
    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    void post(u32 count = 1) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            count_ += count;
        }
        if (count == 1) {
            cv_.notify_one();
        } else {
            cv_.notify_all();
        }
    }

    void acquire() noexcept {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return count_ > 0; });
        --count_;
    }

    [[nodiscard]] bool try_acquire() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ == 0) {
            return false;
        }
        --count_;
        return true;
    }

    /// Returns false on timeout.
    [[nodiscard]] bool try_acquire_for(i64 timeout_ns) noexcept {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::nanoseconds(timeout_ns),
                          [this] { return count_ > 0; })) {
            return false;
        }
        --count_;
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    u32 count_;
};

/// A manual-reset event: set once, every waiter proceeds, and a waiter arriving afterwards does not
/// block. This is the shape a "the frame is published" or "the fence signalled" notification wants;
/// an auto-reset event is a semaphore with a capacity of one, which Semaphore already is.
///
/// LIFETIME. An Event must outlive every call to `set()` that can reach it — including the tail of
/// one that has already woken its waiter. Putting one on the stack of the thread that waits on it
/// and letting the waiter return is therefore wrong, however natural it looks: the setter may still
/// be inside the notification when the frame it lives in goes away. Own the event somewhere that
/// outlives both sides; `CommandQueue` keeps a pool of them for exactly this reason.
class Event {
public:
    Event() noexcept = default;
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;

    void set() noexcept {
        // Notifying with the lock held, not after releasing it. It costs a waiter one immediate
        // re-block in exchange for the setter being finished with the object by the time any
        // waiter can observe the flag, which is what makes reuse of a pooled event safe.
        std::lock_guard<std::mutex> lock(mutex_);
        signalled_ = true;
        cv_.notify_all();
    }

    void reset() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        signalled_ = false;
    }

    [[nodiscard]] bool is_set() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return signalled_;
    }

    void wait() noexcept {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return signalled_; });
    }

    /// Returns false on timeout.
    [[nodiscard]] bool wait_for(i64 timeout_ns) noexcept {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::nanoseconds(timeout_ns),
                            [this] { return signalled_; });
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool signalled_ = false;
};

/// An OS thread. The engine creates almost none of these — `core-jobs-and-concurrency` reserves
/// thread creation to the job system and the documented dedicated threads — and this type exists so
/// that the few that are created are created one way, and named.
class Thread {
public:
    Thread() noexcept = default;

    template <class Fn>
    Thread(const char* name, Fn&& body) : name_(name), thread_(std::forward<Fn>(body)) {}

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    Thread(Thread&&) noexcept = default;
    Thread& operator=(Thread&&) noexcept = default;

    ~Thread() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] bool joinable() const noexcept { return thread_.joinable(); }
    void join() noexcept {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] const char* name() const noexcept { return name_; }

    /// How many threads the machine can run at once, never zero: a platform that reports nothing is
    /// treated as single-core rather than as zero-core, which would give a worker count of −1.
    static u32 hardware_concurrency() noexcept {
        const unsigned reported = std::thread::hardware_concurrency();
        return reported == 0 ? 1u : static_cast<u32>(reported);
    }

    static void yield() noexcept { std::this_thread::yield(); }
    static void sleep_for_ns(i64 nanoseconds) noexcept {
        std::this_thread::sleep_for(std::chrono::nanoseconds(nanoseconds));
    }

private:
    const char* name_ = "";
    std::thread thread_;
};

}  // namespace cy::jobs
