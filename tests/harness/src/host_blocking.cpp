// SPDX-License-Identifier: MIT
// THE FOURTH CLOCK: time the case's thread spent in an UNINTERRUPTIBLE wait. A DIAGNOSTIC, AND
// NOTHING ELSE: it explains a stall and never excuses one.
//
// WHY IT EXISTS. The stall ceiling subtracts runqueue wait (budget.cpp, M9 task 7.5b), which is
// what a CPU-saturated machine costs a case. It does not see what an I/O-saturated machine costs,
// and M11.c's fourth close measured that: `unit.determinism`'s first case held the suite for 655.4
// ms of wall clock with 0.21 ms of CPU and **0 ms** waiting for a core, against a 234.9 ms ceiling,
// during a ledger run beside a heavy build. A case that is doing nothing but touching its own code
// for the first time takes MAJOR PAGE FAULTS when a build has pushed the test binary out of the
// page cache, and on this project's host a fault queued behind a linker's writeback waits hundreds
// of milliseconds. That time is off-CPU and not runnable, and it reads `D` in
// `/proc/self/task/<tid>/stat`: a thread waiting for a block device — a read, a major page fault,
// writeback throttling — sleeps UNINTERRUPTIBLY, while a futex, a nanosleep, a join and a pipe
// read sleep interruptibly and read `S`. A sampler thread reads the case thread's state while the
// case runs, and the time it observes in `D` is this clock.
//
// ================================================================================================
// WHY IT EXCUSES NOTHING: M11.c'S FIFTH, SIXTH AND SEVENTH CLOSES
// ================================================================================================
//
// The first version subtracted every `D` interval from the wall clock. The gate refuted it with one
// probe: a case whose OWN `vfork` child held it for 300 ms on an idle host was reported `contended`
// and passed. The second version excused at most the host's I/O pressure (`/proc/pressure/io`)
// less the calling thread's own share of it; the gate refuted that with a case whose own sixteen
// threads made the pressure, 212 ms excused on a quiet host. The third version took a census of
// the case's whole process tree and subtracted its share too; the gate refuted THAT with a case
// whose helpers were double-forked and re-parented to init, or nested deeper than the census
// walked, 208 ms excused through the real guard.
//
// THE OWNER'S DECISION, after the third refutation: stop patching the allowance. Nothing a process
// can read about itself says what the rest of the machine was doing to it — an uninterruptible
// wait says the kernel made the case wait and never says what for, and every instrument that tried
// to attribute it was gamed by a case that made the wait itself. So the harness EXCUSES NO
// UNINTERRUPTIBLE WAIT AT ALL. The premise that the host is quiet is stated where it belongs, in
// the ledger criteria that run timing-sensitive suites, and checked from OUTSIDE the process:
// `tools/quiet-host/` waits for a quiet host, runs the suite, judges the host across the run and
// fails with "host too busy:" when it was not. A case over its ceiling on a quiet host is the
// case's own, whatever it was waiting for.
//
// What is left of the fourth clock is its diagnostic value: the stall message says how long the
// case was in an uninterruptible wait, so a stall behind a build's writeback is explained rather
// than mistaken for a sleep. The verdict does not read it.
//
// ITS LIMITS, SAID PLAINLY.
//  * It is a SAMPLE, not an account: the interval is between one and five milliseconds (budget.cpp
//    chooses it from the ceiling), each interval is credited by the trapezoid rule, and the last
//    interval before the case ends is not credited at all.
//  * `D` is every uninterruptible sleep, not only I/O: a kernel mutex, the `mmap_lock`, a `vfork`.
//  * One thread at a time, as the CLOCK. Work a case hands to another thread is not sampled as its
//    own waiting, exactly as it is not counted by the CPU clock.
//  * Linux only. Elsewhere `begin` returns false and the message says "not measured here".

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
    /// thread has exited — reads as not blocked.
    [[nodiscard]] bool blocked() const noexcept {
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

/// Credits one sampling interval by the trapezoid rule — all of it when the thread was blocked at
/// both ends, half when at one end — which is unbiased for a wait that begins or ends inside it.
std::uint64_t credit(bool previous, bool current, std::uint64_t elapsed) noexcept {
    if (previous && current) {
        return elapsed;
    }
    return (previous || current) ? elapsed / 2 : 0;
}

/// What the sampler thread needs to know about the session it is sampling.
struct SessionPlan {
    pid_t tid = 0;
    std::uint64_t generation = 0;
    unsigned long long interval_ns = 0;
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
        plan_ = SessionPlan{tid, ++generation_, interval_ns};
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
            const SessionPlan plan = plan_;
            lock.unlock();
            sample(plan);
            lock.lock();
        }
    }

    [[nodiscard]] bool session_over_locked(std::uint64_t generation) const noexcept {
        return stop_ || generation_ != generation;
    }

    /// One case: sample until the generation moves on.
    void sample(const SessionPlan& plan) {
        const ThreadStat stat(plan.tid);
        std::unique_lock<std::mutex> lock(mutex_);
        const auto over = [&]() noexcept { return session_over_locked(plan.generation); };
        if (!stat.valid()) {
            wake_.wait(lock, over);
            return;
        }
        lock.unlock();
        bool previous = stat.blocked();
        std::uint64_t previous_ns = monotonic_ns();
        lock.lock();
        while (!over()) {
            wake_.wait_for(lock, std::chrono::nanoseconds(plan.interval_ns), over);
            if (over()) {
                return;
            }
            lock.unlock();
            const bool current = stat.blocked();
            const std::uint64_t now_ns = monotonic_ns();
            const std::uint64_t credited = credit(previous, current, now_ns - previous_ns);
            previous = current;
            previous_ns = now_ns;
            lock.lock();
            if (generation_ == plan.generation) {
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
    std::uint64_t generation_ = 0;
    SessionPlan plan_;
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
