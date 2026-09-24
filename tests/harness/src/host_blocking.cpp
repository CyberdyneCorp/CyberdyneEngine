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
//    stalled on I/O moves it by the whole window whether or not this case touched the disk. ALONE
//    it under-excuses the flake and over-excuses a sleeping case at the same time. It is used below
//    for what it IS good at: saying how much the rest of the host was stalled, as a second bound.
//  * THE THREAD'S OWN SCHEDULER STATE (`/proc/self/task/<tid>/stat` field 3). Always available to
//    the process itself, per thread, and it separates exactly the two things the ceiling has to
//    separate. A thread waiting on a futex (a mutex, a condition variable, a join), in `nanosleep`,
//    or in `poll`/`read` on a pipe or socket sleeps INTERRUPTIBLY and reads `S`. A thread waiting
//    for a block device — a read, a major page fault, writeback throttling — sleeps
//    UNINTERRUPTIBLY, in `io_schedule()`, and reads `D`.
//
// So a sampler thread reads the case thread's state while the case runs, and the time it observes
// in `D` is the fourth clock. That is the first half of this file; the second half, below, is why
// that clock is only an upper bound on what the guard excuses.
//
// ITS LIMITS, SAID PLAINLY.
//  * It is a SAMPLE, not an account: the interval is between one and five milliseconds (budget.cpp
//    chooses it from the ceiling), each interval is credited by the trapezoid rule, and the last
//    interval before the case ends is not credited at all. The error is a few intervals against a
//    ceiling of hundreds — and the missing interval errs towards FAILING, never towards excusing.
//  * `D` is every uninterruptible sleep, not only I/O: a kernel mutex, the `mmap_lock`, a `vfork`.
//  * One thread at a time. Work a case hands to another thread is not sampled, exactly as it is not
//    counted by the CPU clock; a case that waits for that work blocks on a futex and reads `S`.
//  * Linux only. Elsewhere `begin` returns None and the ceiling behaves exactly as before.
//
// ================================================================================================
// M11.c'S FIFTH CLOSE: `D` IS AN UPPER BOUND, NOT AN EXCUSE
// ================================================================================================
//
// The first version subtracted every `D` interval from the wall clock, and the gate that refuted it
// needed one line to show why that is wrong: a case whose OWN `vfork` child held it for 300 ms on
// an idle host was failed as `stalled:` before and reported as `contended:` after. So is a case's
// own `fsync`, and its own uncached reads. An uninterruptible wait says the kernel made the case
// wait; it does not say what the kernel was waiting FOR, and on a quiet host that is the case
// itself.
//
// THE OWNER'S RULE: the stall allowance may excuse only waiting the HOST causes. What says the host
// was busy with I/O is PRESSURE STALL INFORMATION, `/proc/pressure/io`, whose `some` total grows
// while at least one task somewhere is stalled on I/O. It is host-wide, available without root on
// every kernel this project runs on, and it does not move for a wait that is not I/O: measured
// over 300 ms windows on this project's host, a `vfork` wait moves it by 0 ms on a quiet host.
//
// But it moves for the case's OWN I/O too, so it cannot be used as it stands. PSI is an average
// over processors weighted by each one's non-idle time, and a processor counts as stalled while any
// task queued on it is. One thread in `D` for `blocked` ms therefore puts at most
// `blocked × nonidle_of_its_cpu / Σ nonidle` into the total — measured: 205 ms of the case's own
// O_DIRECT reads on a host with 24 busy processors moved it 7.9 ms, and the bound says 8.5; 288 ms
// of its own fsync moved it 11.8, bound 12.0. The case's processor is non-idle for at most the
// window, so `host_stall_allowance` subtracts `blocked × window / Σ nonidle` — `Σ nonidle` read
// from /proc/stat over the same window and rounded down by its tick resolution — and calls what is
// left the host's. It excuses the smaller of that and `blocked`.
//
// WHAT THAT MEANS, SAID PLAINLY.
//  * A case that blocks itself on an otherwise quiet host is excused nothing, and fails.
//  * A case whose waiting sat behind other processes' I/O is excused at most what those processes
//    were stalled, IN PSI'S UNITS. Those are averaged over the machine: on a 24-processor host
//    whose every processor is compiling, one other task stalled for the whole window moves `some`
//    by about a twenty-fourth of it. So a heavily loaded but lightly I/O-bound host excuses little,
//    and a case can still fail as stalled there. That is the rule doing what it says — only
//    pressure the host shows is excused — and not a measurement error.
//  * The bound is on the MAGNITUDE the host was stalled, not on WHICH wait. A case that blocks
//    itself (a vfork) while other processes happen to be stalled on I/O is excused up to their
//    stall time, even though it did not wait for them. On a quiet host there is nothing to excuse.
//  * Work the kernel does on the case's behalf in another task — `jbd2` committing the case's own
//    fsync, a kworker writing back its own dirty pages — counts as another task's.
//  * PSI's per-processor weights are whole jiffies, so a period between two reads shorter than a
//    few jiffies loses precision (and every read, by anyone, closes a period). The pressure files
//    are therefore read only by a case that has already run `baseline_delay_ns` — an eighth of its
//    ceiling, 20 to 100 ms — and once more at its end only if it is over its ceiling. The
//    waiting before the baseline is not excusable at all, which errs towards failing.
//  * No /proc/pressure/io, or no /proc/stat: the allowance is ZERO. The ceiling then charges every
//    uninterruptible wait to the case, as it did before the fourth clock existed.

#include "host_blocking.h"

#include <cy/test/test.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#if defined(__linux__)
#    include <chrono>
#    include <condition_variable>
#    include <csignal>
#    include <cstdio>
#    include <cstdlib>
#    include <cstring>
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

namespace {

/// The most a single thread blocked for `blocked_ns` of a `window_ns` window can have added to
/// PSI's `some` total, given that the host's processors were non-idle for `nonidle_ns` in total
/// over the same window, read to within `resolution_ns`.
///
/// PSI weights each processor's stall time by that processor's non-idle time and divides by the
/// sum. The case's processor is non-idle for at most the window, so its weight is at most
/// `window / Σ nonidle`. Every rounding goes against the case: the non-idle sum is taken at the
/// bottom of its resolution, never below the window itself (which would claim the case's own
/// processor was idle while it was stalled on it), and the product is rounded up.
unsigned long long own_share_ceiling(unsigned long long window_ns, unsigned long long blocked_ns,
                                     unsigned long long nonidle_ns,
                                     unsigned long long resolution_ns) noexcept {
    const unsigned long long lowest = nonidle_ns > resolution_ns ? nonidle_ns - resolution_ns : 0;
    const unsigned long long weight_base = std::max(lowest, window_ns);
    const double share = static_cast<double>(blocked_ns) *
                         (static_cast<double>(window_ns) / static_cast<double>(weight_base));
    return static_cast<unsigned long long>(std::ceil(share));
}

}  // namespace

unsigned long long host_stall_allowance(unsigned long long window_ns, unsigned long long blocked_ns,
                                        const HostPressure& before,
                                        const HostPressure& after) noexcept {
    // No reading, no allowance: an instrument that cannot say the host was busy excuses nothing.
    if (!before.available || !after.available || window_ns == 0 || blocked_ns == 0) {
        return 0;
    }
    // Two readings out of order, or of a counter that was reset, are not a measurement.
    if (after.io_some_ns < before.io_some_ns || after.nonidle_cpu_ns < before.nonidle_cpu_ns) {
        return 0;
    }
    const unsigned long long pressure = after.io_some_ns - before.io_some_ns;
    const unsigned long long own =
        own_share_ceiling(window_ns, blocked_ns, after.nonidle_cpu_ns - before.nonidle_cpu_ns,
                          std::max(before.nonidle_resolution_ns, after.nonidle_resolution_ns));
    const unsigned long long others = pressure > own ? pressure - own : 0;
    return std::min(blocked_ns, others);
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

/// The first `size - 1` bytes of a /proc file, NUL-terminated. False when it cannot be read, which
/// is also what a kernel booted with `psi=0` answers for /proc/pressure/io (EOPNOTSUPP).
bool read_head(const char* path, char* buffer, std::size_t size) noexcept {
    const int descriptor = ::open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return false;
    }
    // No caller holds the sampler's mutex here: `sample` releases it before `take_baseline`, which
    // the analyzer loses track of across the condition-variable wait that re-takes it.
    // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection)
    const ::ssize_t got = ::read(descriptor, buffer, size - 1);
    ::close(descriptor);
    if (got <= 0) {
        return false;
    }
    buffer[static_cast<std::size_t>(got)] = '\0';
    return true;
}

/// "some avg10=... avg60=... avg300=... total=<us>": the `some` line's total, in microseconds.
bool parse_io_some_total_us(const char* text, unsigned long long& total_us) noexcept {
    if (std::strncmp(text, "some ", 5) != 0) {
        return false;
    }
    const char* line_end = std::strchr(text, '\n');
    const char* total = std::strstr(text, "total=");
    if (total == nullptr || (line_end != nullptr && total > line_end)) {
        return false;
    }
    const char* digits = total + 6;
    char* after = nullptr;
    total_us = std::strtoull(digits, &after, 10);
    return after != digits;
}

/// The fields of /proc/stat's aggregate "cpu" line that count as NOT idle to PSI: user, nice,
/// system, iowait, irq, softirq and steal (guest time is already inside user). `idle` is the one
/// left out. A processor with a task in iowait is non-idle to PSI, so iowait is in.
constexpr int kStatFields = 8;
constexpr int kIdleField = 3;
constexpr int kNonidleFields = kStatFields - 1;

bool parse_nonidle_ticks(const char* text, unsigned long long& ticks) noexcept {
    if (std::strncmp(text, "cpu ", 4) != 0) {
        return false;
    }
    const char* cursor = text + 4;
    ticks = 0;
    for (int field = 0; field < kStatFields; ++field) {
        char* after = nullptr;
        const unsigned long long value = std::strtoull(cursor, &after, 10);
        if (after == cursor) {
            return false;
        }
        if (field != kIdleField) {
            ticks += value;
        }
        cursor = after;
    }
    return true;
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

/// Credits one sampling interval by the trapezoid rule — all of it when the thread was blocked at
/// both ends, half when at one end — which is unbiased for a wait that begins or ends inside it.
std::uint64_t credit(bool previous, bool current, std::uint64_t elapsed) noexcept {
    if (previous && current) {
        return elapsed;
    }
    return (previous || current) ? elapsed / 2 : 0;
}

/// Where the pressure window starts: the host's pressure, and the fourth clock's reading, at the
/// moment the session had run long enough to be worth measuring.
struct Baseline {
    bool taken = false;
    std::uint64_t at_ns = 0;
    unsigned long long blocked_ns = 0;
    HostPressure pressure;
};

/// What the sampler thread needs to know about the session it is sampling.
struct SessionPlan {
    pid_t tid = 0;
    std::uint64_t generation = 0;
    unsigned long long interval_ns = 0;
    std::uint64_t started_ns = 0;
    unsigned long long baseline_delay_ns = 0;
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

    host_blocking::Session begin(pid_t tid, unsigned long long interval_ns,
                                 unsigned long long baseline_delay_ns) noexcept {
        std::unique_lock<std::mutex> lock(mutex_);
        if (target_ == tid) {
            ++depth_;
            return host_blocking::Session::Nested;
        }
        if (target_ != 0 || !start_locked()) {
            return host_blocking::Session::None;
        }
        target_ = tid;
        depth_ = 1;
        plan_ = SessionPlan{tid, ++generation_, interval_ns, monotonic_ns(), baseline_delay_ns};
        baseline_ = Baseline{};
        lock.unlock();
        wake_.notify_one();
        return host_blocking::Session::Owner;
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

    unsigned long long allowance(pid_t tid) noexcept {
        Baseline baseline;
        unsigned long long blocked_now = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (target_ != tid || depth_ != 1 || !baseline_.taken) {
                return 0;
            }
            baseline = baseline_;
            blocked_now = observed_ns_;
        }
        const HostPressure now = host_pressure_now();
        const std::uint64_t now_ns = monotonic_ns();
        return host_stall_allowance(now_ns - baseline.at_ns, blocked_now - baseline.blocked_ns,
                                    baseline.pressure, now);
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

    /// The pressure baseline, taken on this thread rather than the case's so that reading two /proc
    /// files costs the case nothing. Stored only if the session it was taken for is still running.
    void take_baseline(const SessionPlan& plan) noexcept {
        const HostPressure pressure = host_pressure_now();
        const std::uint64_t at_ns = monotonic_ns();
        const std::lock_guard<std::mutex> lock(mutex_);
        if (generation_ == plan.generation) {
            baseline_ = Baseline{true, at_ns, observed_ns_, pressure};
        }
    }

    /// One case: sample until the generation moves on, and take the pressure baseline once the
    /// case has run long enough for its window to be measurable.
    void sample(const SessionPlan& plan) {
        const ThreadStat stat(plan.tid);
        std::unique_lock<std::mutex> lock(mutex_);
        const auto over = [&]() noexcept { return session_over_locked(plan.generation); };
        if (!stat.valid()) {
            wake_.wait(lock, over);
            return;
        }
        lock.unlock();
        bool previous = stat.blocked_by_host();
        std::uint64_t previous_ns = monotonic_ns();
        bool baseline_taken = false;
        lock.lock();
        while (!over()) {
            wake_.wait_for(lock, std::chrono::nanoseconds(plan.interval_ns), over);
            if (over()) {
                return;
            }
            lock.unlock();
            const bool current = stat.blocked_by_host();
            const std::uint64_t now_ns = monotonic_ns();
            const std::uint64_t credited = credit(previous, current, now_ns - previous_ns);
            previous = current;
            previous_ns = now_ns;
            lock.lock();
            if (generation_ == plan.generation) {
                observed_ns_ += credited;
            }
            if (!baseline_taken && now_ns - plan.started_ns >= plan.baseline_delay_ns) {
                baseline_taken = true;
                lock.unlock();
                take_baseline(plan);
                lock.lock();
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
    Baseline baseline_;
    /// Cumulative across every session, and only ever grown for the thread that owns the current
    /// one: a guard reads it at both ends of its own session and takes the difference.
    unsigned long long observed_ns_ = 0;
};

}  // namespace

namespace host_blocking {

Session begin(unsigned long long interval_ns, unsigned long long baseline_delay_ns) noexcept {
    if (!budget_measures_host_blocking()) {
        return Session::None;
    }
    return Sampler::instance().begin(current_tid(), interval_ns, baseline_delay_ns);
}

unsigned long long allowance() noexcept {
    return Sampler::instance().allowance(current_tid());
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

HostPressure host_pressure_now() noexcept {
    HostPressure reading;
    char pressure[256];
    unsigned long long some_us = 0;
    if (!read_head("/proc/pressure/io", pressure, sizeof(pressure)) ||
        !parse_io_some_total_us(pressure, some_us)) {
        return reading;
    }
    // Only the aggregate first line is wanted; the kernel builds the whole file either way.
    char stat[512];
    unsigned long long nonidle_ticks = 0;
    if (!read_head("/proc/stat", stat, sizeof(stat)) || !parse_nonidle_ticks(stat, nonidle_ticks)) {
        return reading;
    }
    const long ticks_per_second = ::sysconf(_SC_CLK_TCK);
    if (ticks_per_second <= 0) {
        return reading;
    }
    const auto tick_ns = 1'000'000'000ULL / static_cast<unsigned long long>(ticks_per_second);
    reading.available = true;
    reading.io_some_ns = some_us * 1'000ULL;
    reading.nonidle_cpu_ns = nonidle_ticks * tick_ns;
    // Each field is summed over processors in nanoseconds and truncated to a tick once, so the
    // difference of two readings is off by less than one tick per field.
    reading.nonidle_resolution_ns = static_cast<unsigned long long>(kNonidleFields) * tick_ns;
    return reading;
}

bool budget_measures_host_pressure() noexcept {
    static const bool available = host_pressure_now().available;
    return available;
}

#else  // !__linux__

namespace host_blocking {

Session begin(unsigned long long /*interval_ns*/,
              unsigned long long /*baseline_delay_ns*/) noexcept {
    return Session::None;
}

unsigned long long allowance() noexcept {
    return 0;
}

void end() noexcept {}

}  // namespace host_blocking

unsigned long long blocked_on_host_ns() noexcept {
    return 0;
}

bool budget_measures_host_blocking() noexcept {
    return false;
}

HostPressure host_pressure_now() noexcept {
    return HostPressure{};
}

bool budget_measures_host_pressure() noexcept {
    return false;
}

#endif

}  // namespace cy::test
