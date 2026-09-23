// SPDX-License-Identifier: MIT
// THE FOURTH CLOCK: time the case's thread spent BLOCKED BY THE HOST, as opposed to by itself.
//
// WHY IT EXISTS. The stall ceiling subtracts runqueue wait (budget.cpp, M9 task 7.5b), which is
// what a CPU-saturated machine costs a case. It does not see what an I/O-saturated machine costs,
// and M11.c's fourth close measured that: `unit.determinism`'s first case held the suite for 655.4
// ms of wall clock with 0.21 ms of CPU and **0 ms** waiting for a core, against a 234.9 ms ceiling,
// during a ledger run beside a heavy build. It passed three runs in three alone. A case that is
// doing nothing but touching its own code for the first time takes MAJOR PAGE FAULTS when a build
// has pushed the test binary out of the page cache, and on this project's host (one SATA SSD shared
// with every build tree) a fault queued behind a linker's writeback waits hundreds of milliseconds.
// That time is off-CPU and not runnable, so the stall ceiling charged it to the case.
//
// WHAT LINUX EXPOSES, AND WHY THIS IS THE ONE USED. Three instruments were considered:
//
//  * DELAY ACCOUNTING (`/proc/thread-self/stat` field 42, `delayacct_blkio_ticks`). Exact, per
//    thread, and the right answer where it is on — but it is OFF by default since Linux 5.14
//    (`kernel.task_delayacct = 0`; it is 0 on this host), turning it on needs root, and when off
//    the field reads zero rather than failing. An instrument that silently reads zero on every
//    machine this project runs on would excuse nothing and look as if it worked.
//  * PRESSURE STALL INFORMATION (`/proc/pressure/io`, or the cgroup's `io.pressure`). Available,
//    but it is not per thread: its `some` total is an average of every CPU's stall time WEIGHTED BY
//    THAT CPU'S NON-IDLE TIME, so on a machine whose other 23 cores are busy compiling, one thread
//    blocked for 600 ms moves `some` by about 25 ms — and a machine whose OTHER processes are
//    stalled on I/O moves it by the whole window whether or not this case touched the disk. It
//    under-excuses the flake and over-excuses a sleeping case at the same time.
//  * THE THREAD'S OWN SCHEDULER STATE (`/proc/self/task/<tid>/stat` field 3). Always available to
//    the process itself, per thread, and it separates exactly the two things the ceiling has to
//    separate. A thread waiting on a futex (a mutex, a condition variable, a join), in `nanosleep`,
//    or in `poll`/`read` on a pipe or socket sleeps INTERRUPTIBLY and reads `S`. A thread waiting
//    for a block device — a read, a major page fault, writeback throttling — sleeps
//    UNINTERRUPTIBLY, in `io_schedule()`, and reads `D`.
//
// So a sampler thread reads the case thread's state while the case runs, and the time it observes
// in `D` is the fourth clock. That is what this file is.
//
// ITS LIMITS, SAID PLAINLY.
//  * It is a SAMPLE, not an account: the interval is between one and five milliseconds (budget.cpp
//    chooses it from the ceiling), each interval is credited by the trapezoid rule, and the last
//    interval before the case ends is not credited at all. The error is a few intervals against a
//    ceiling of hundreds — and the missing interval errs towards FAILING, never towards excusing.
//  * `D` is every uninterruptible sleep, not only I/O: a kernel mutex, the `mmap_lock`, a `vfork`.
//    Each of those is the kernel making the case wait for something outside it, which is the class
//    the ceiling means to excuse; none of them is a sleep, a lock or a join the case wrote.
//  * A case that does its OWN synchronous disk I/O — an `fsync` in a unit test — is excused for it.
//    `testing-and-quality` puts I/O in tests/integration/ or above, whose ceilings are 100 s and
//    more; the unit tier's ceiling is not the instrument that enforces that rule.
//  * One thread at a time. Work a case hands to another thread is not sampled, exactly as it is not
//    counted by the CPU clock; a case that waits for that work blocks on a futex and reads `S`.
//  * Linux only. Elsewhere `begin` returns false and the ceiling behaves exactly as before.

#include "host_blocking.h"

#include <cy/test/test.h>

#include <cstddef>
#include <cstdint>

#if defined(__linux__)
#    include <chrono>
#    include <condition_variable>
#    include <csignal>
#    include <cstdio>
#    include <ctime>
#    include <mutex>

#    include <fcntl.h>
#    include <pthread.h>
#    include <sys/syscall.h>
#    include <unistd.h>
#endif

namespace cy::test {

char thread_state_from_stat(const char* text, std::size_t size) noexcept {
    // "pid (comm) S ppid ...": `comm` is up to sixteen bytes of anything the thread chose to call
    // itself, parentheses and spaces included, so the state is found after the LAST ')' — the
    // kernel's own `proc_pid_stat` gives no other delimiter that survives a hostile name.
    if (text == nullptr) {
        return '\0';
    }
    std::size_t close = size;
    while (close > 0 && text[close - 1] != ')') {
        --close;
    }
    // `close` is one past the ')' now, or zero when there was none.
    if (close == 0 || close + 1 >= size || text[close] != ' ') {
        return '\0';
    }
    return text[close + 1];
}

#if defined(__linux__)

namespace {

std::uint64_t monotonic_ns() noexcept {
    timespec now{};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return (static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL) +
           static_cast<std::uint64_t>(now.tv_nsec);
}

pid_t current_tid() noexcept {
    static thread_local const auto tid = static_cast<pid_t>(::syscall(SYS_gettid));
    return tid;
}

/// One thread's `stat`, held open for a session and re-read from offset zero on every sample:
/// /proc regenerates it on each read, as it does the schedstat file budget.cpp reads.
class ThreadStat {
public:
    explicit ThreadStat(pid_t tid) noexcept {
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", static_cast<int>(tid));
        descriptor_ = ::open(path, O_RDONLY | O_CLOEXEC);
    }
    ~ThreadStat() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }
    ThreadStat(const ThreadStat&) = delete;
    ThreadStat& operator=(const ThreadStat&) = delete;

    [[nodiscard]] bool valid() const noexcept { return descriptor_ >= 0; }

    /// True when the thread is in an uninterruptible sleep right now. A read that fails — the
    /// thread has exited — reads as not blocked, which is the direction that excuses nothing.
    [[nodiscard]] bool blocked_by_host() const noexcept {
        char buffer[512];
        const ::ssize_t got = ::pread(descriptor_, buffer, sizeof(buffer), 0);
        if (got <= 0) {
            return false;
        }
        return thread_state_from_stat(buffer, static_cast<std::size_t>(got)) == 'D';
    }

private:
    int descriptor_ = -1;
};

/// The sampler. One per process, started by the first case and parked on a condition variable
/// whenever no case is running, so a binary between cases costs nothing.
class Sampler {
public:
    static Sampler& instance() noexcept {
        static Sampler sampler;
        return sampler;
    }

    Sampler() = default;
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    ~Sampler() {
        // A child of `fork()` inherits this object but not the thread, and possibly a mutex the
        // thread held at the moment of the fork. Touching either would hang the child, so a process
        // that did not start the thread leaves it alone.
        if (!started_ || owner_ != ::getpid()) {
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        ::pthread_join(thread_, nullptr);
    }

    bool begin(pid_t tid, unsigned long long interval_ns) noexcept {
        std::unique_lock<std::mutex> lock(mutex_);
        if (target_ == tid) {
            ++depth_;
            return true;
        }
        if (target_ != 0 || !start_locked()) {
            return false;
        }
        target_ = tid;
        depth_ = 1;
        interval_ns_ = interval_ns;
        ++generation_;
        lock.unlock();
        wake_.notify_one();
        return true;
    }

    void end(pid_t tid) noexcept {
        // No notification: the sampler notices the new generation at its next interval and parks.
        // Waking it here would cost the case a futex call for nothing.
        const std::lock_guard<std::mutex> lock(mutex_);
        if (target_ != tid || depth_ == 0) {
            return;
        }
        if (--depth_ == 0) {
            target_ = 0;
            ++generation_;
        }
    }

    unsigned long long observed_ns(pid_t tid) noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        return target_ == tid ? observed_ns_ : 0ULL;
    }

private:
    bool start_locked() noexcept {
        if (started_) {
            return true;
        }
        // The sampler must never be the thread a process-directed signal lands on — a test that
        // waits for SIGCHLD or SIGALRM with `sigwait` would lose it — so every signal is blocked in
        // the thread that creates it, which it inherits, and then restored here.
        sigset_t all;
        sigset_t previous;
        ::sigfillset(&all);
        ::pthread_sigmask(SIG_SETMASK, &all, &previous);
        const int created = ::pthread_create(&thread_, nullptr, &Sampler::entry, this);
        ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        if (created != 0) {
            return false;
        }
        started_ = true;
        owner_ = ::getpid();
        return true;
    }

    static void* entry(void* self) {
        static_cast<Sampler*>(self)->run();
        return nullptr;
    }

    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_) {
            if (target_ == 0) {
                wake_.wait(lock);
                continue;
            }
            const pid_t tid = target_;
            const std::uint64_t generation = generation_;
            const unsigned long long interval_ns = interval_ns_;
            lock.unlock();
            sample(tid, generation, interval_ns);
            lock.lock();
        }
    }

    [[nodiscard]] bool session_over_locked(std::uint64_t generation) const noexcept {
        return stop_ || generation_ != generation;
    }

    /// One case: sample until the generation moves on. Credits each interval by the trapezoid rule
    /// — all of it when the thread was blocked at both ends, half when at one end — which is
    /// unbiased for a wait that begins or ends inside the interval.
    void sample(pid_t tid, std::uint64_t generation, unsigned long long interval_ns) {
        const ThreadStat stat(tid);
        std::unique_lock<std::mutex> lock(mutex_);
        const auto over = [&]() noexcept { return session_over_locked(generation); };
        if (!stat.valid()) {
            wake_.wait(lock, over);
            return;
        }
        lock.unlock();
        bool previous = stat.blocked_by_host();
        std::uint64_t previous_ns = monotonic_ns();
        lock.lock();
        while (!over()) {
            wake_.wait_for(lock, std::chrono::nanoseconds(interval_ns), over);
            if (over()) {
                return;
            }
            lock.unlock();
            const bool current = stat.blocked_by_host();
            const std::uint64_t now_ns = monotonic_ns();
            const std::uint64_t elapsed = now_ns - previous_ns;
            std::uint64_t credited = 0;
            if (previous && current) {
                credited = elapsed;
            } else if (previous || current) {
                credited = elapsed / 2;
            }
            previous = current;
            previous_ns = now_ns;
            lock.lock();
            if (generation_ == generation) {
                observed_ns_ += credited;
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    pthread_t thread_{};
    bool started_ = false;
    pid_t owner_ = 0;
    bool stop_ = false;
    pid_t target_ = 0;
    unsigned depth_ = 0;
    unsigned long long interval_ns_ = 0;
    std::uint64_t generation_ = 0;
    /// Cumulative across every session, and only ever grown for the thread that owns the current
    /// one: a guard reads it at both ends of its own session and takes the difference.
    unsigned long long observed_ns_ = 0;
};

}  // namespace

namespace host_blocking {

bool begin(unsigned long long interval_ns) noexcept {
    if (!budget_measures_host_blocking()) {
        return false;
    }
    return Sampler::instance().begin(current_tid(), interval_ns);
}

void end() noexcept {
    Sampler::instance().end(current_tid());
}

}  // namespace host_blocking

unsigned long long blocked_on_host_ns() noexcept {
    if (!budget_measures_host_blocking()) {
        return 0;
    }
    return Sampler::instance().observed_ns(current_tid());
}

bool budget_measures_host_blocking() noexcept {
    // Not "this is Linux": /proc can be absent in a container. Checked once, on the thread that
    // asks first; every thread of the process has the same /proc.
    static const bool available = []() noexcept {
        const int descriptor = ::open("/proc/thread-self/stat", O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) {
            return false;
        }
        char buffer[512];
        const ::ssize_t got = ::read(descriptor, buffer, sizeof(buffer));
        ::close(descriptor);
        return got > 0 && thread_state_from_stat(buffer, static_cast<std::size_t>(got)) != '\0';
    }();
    return available;
}

#else  // !__linux__

namespace host_blocking {

bool begin(unsigned long long /*interval_ns*/) noexcept {
    return false;
}

void end() noexcept {}

}  // namespace host_blocking

unsigned long long blocked_on_host_ns() noexcept {
    return 0;
}

bool budget_measures_host_blocking() noexcept {
    return false;
}

#endif

}  // namespace cy::test
