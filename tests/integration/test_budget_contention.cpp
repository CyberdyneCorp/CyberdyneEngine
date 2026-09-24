// The other half of M9 task 7.5b's proof: a case preempted by a busy machine IS excused.
//
// `tests/unit/harness/test_budget.cpp` holds the two halves that need no machine state — the stall
// verdict as a pure function of three numbers, and the negative control that a case which BLOCKS
// accumulates no runqueue wait, so a sleep is still a stall. This is the positive one, and it is in
// `integration` rather than `unit` for the reason the taxonomy gives: it oversubscribes a core for
// a tenth of a second, which is a hundred times the unit tier's budget.
//
// WHY IT MATTERS. `smoke.editor_window`, `unit.render_gpu_culling` and the editor's
// `across_a_process_boundary` each failed ONCE across three full ledger runs under the ledger's own
// sustained load, and each passed three to five times in isolation on an idle machine. M8.c's
// closing gate recorded that as task 7.5b and could not fix it inside its own scope: "a gate that
// is red one run in three teaches people to re-run it". The fix is the third clock, and this is the
// case that says the third clock sees what it is supposed to see.
//
// ================================================================================================
// WHY IT PINS ITSELF TO ONE CORE, WHICH IS THE MEASUREMENT AND NOT A TRICK
// ================================================================================================
//
// The first version created ninety-six spinning threads on a twenty-four core machine and measured
// **zero** contention, repeatably. That is not a bug in the clock: with an idle machine and one
// process, the load balancer gives the measuring thread a core of its own and packs the spinners
// onto the others, so nothing ever waits on a runqueue. The wait this instrument exists to see is
// the one a *busy machine* produces — forty compiler processes in another session — and a test may
// not start forty processes to prove it.
//
// Restricting this thread and its spinners to a single CPU produces exactly that condition inside
// one process, deterministically: nine runnable threads, one core, and the measuring thread spends
// most of its window enqueued. Measured across runs: 69-76 ms of wall clock for a 60 ms window, of
// which 59-66 ms was runqueue wait. The affinity mask is restored afterwards, and a platform that
// refuses to set one reports that rather than asserting on a condition it could not create.

#include <cy/test/fixtures.h>
#include <cy/test/test.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#    include <fcntl.h>
#    include <sched.h>
#    include <spawn.h>
#    include <sys/mman.h>
#    include <sys/resource.h>
#    include <sys/types.h>
#    include <sys/wait.h>
#    include <unistd.h>

#    include <condition_variable>
#    include <csignal>
#    include <cstdint>
#    include <cstdlib>
#    include <cstring>
#    include <ctime>
#    include <mutex>

extern char** environ;  // NOLINT(readability-redundant-declaration): POSIX names it, no header does
#endif

namespace {

#if defined(__linux__)
/// Spin until told to stop, announcing readiness first.
///
/// THE DEADLINE IS NOT SET IN ADVANCE, and the first version of this file got that wrong in a way
/// worth keeping written down: it computed `now + 120 ms` before creating the spinners, and
/// creating them can take longer than that. Every spinner then returned immediately, the
/// measurement window had the machine to itself, and the case failed with 0.004 ms of contention —
/// a test measuring how long a thread constructor takes rather than what it claims to measure. It
/// passed when run alone and failed inside the suite, which is the signature.
void spin_until_stopped(const std::atomic<bool>& stop, std::atomic<unsigned>& ready) {
    ready.fetch_add(1, std::memory_order_relaxed);
    volatile unsigned sink = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        for (unsigned index = 0; index < 1024U; ++index) {
            sink = sink + index;
        }
    }
    (void)sink;
}
#endif

}  // namespace

CY_TEST_CASE("harness: a case preempted by a busy machine accumulates contention, and is excused") {
    if (!cy::test::budget_measures_contention()) {
        CY_TEST_MESSAGE(
            "this platform does not report runqueue wait; the stall ceiling is unchanged");
        return;
    }

#if !defined(__linux__)
    CY_TEST_MESSAGE(
        "this platform cannot pin a thread to one core; the condition cannot be created");
#else
    cpu_set_t original;
    CPU_ZERO(&original);
    if (::sched_getaffinity(0, sizeof(original), &original) != 0) {
        CY_TEST_MESSAGE("the affinity mask could not be read; nothing is asserted");
        return;
    }
    cpu_set_t single;
    CPU_ZERO(&single);
    CPU_SET(0, &single);
    if (::sched_setaffinity(0, sizeof(single), &single) != 0) {
        CY_TEST_MESSAGE(
            "this environment refuses an affinity mask; the condition cannot be created");
        return;
    }

    constexpr unsigned kSpinners = 8;
    std::atomic<bool> stop{false};
    std::atomic<unsigned> ready{0};
    std::vector<std::thread> spinners;
    spinners.reserve(static_cast<std::size_t>(kSpinners));
    for (unsigned index = 0; index < kSpinners; ++index) {
        spinners.emplace_back([&]() { spin_until_stopped(stop, ready); });
    }
    while (ready.load(std::memory_order_relaxed) < kSpinners) {
        std::this_thread::yield();
    }

    // Measure a window of ordinary work while nine runnable threads share one core.
    const unsigned long long contended_before = cy::test::contended_ns();
    const auto started = std::chrono::steady_clock::now();
    volatile unsigned long long sink = 0;
    while (std::chrono::steady_clock::now() < started + std::chrono::milliseconds(60)) {
        sink = sink + 1U;
    }
    const auto wall = std::chrono::steady_clock::now() - started;
    const unsigned long long contended = cy::test::contended_ns() - contended_before;
    (void)sink;

    stop.store(true, std::memory_order_relaxed);
    for (std::thread& spinner : spinners) {
        spinner.join();
    }
    (void)::sched_setaffinity(0, sizeof(original), &original);

    const auto wall_ns = static_cast<unsigned long long>(std::chrono::nanoseconds(wall).count());
    CY_REQUIRE(wall_ns >= 50000000ULL);

    // THE ASSERTION: with nine runnable threads on one core, this thread spent a measurable part of
    // the window wanting a core and not having one. A quarter of the window is a low bar
    // deliberately — the measured figure is four fifths of it — because the claim is that the clock
    // MOVES under preemption, which is what lets the guard subtract it, and not that the scheduler
    // divides time in any particular way.
    CY_CHECK_GT(contended, wall_ns / 4U);
    CY_TEST_MESSAGE("contended for " << (static_cast<double>(contended) / 1e6) << " ms of a "
                                     << (static_cast<double>(wall_ns) / 1e6) << " ms window");
#endif
}

// ================================================================================================
// M11.c's FOURTH AND FIFTH CLOSES: the host's DISK, which the runqueue clock cannot see
// ================================================================================================
//
// `unit.determinism`'s first case held the suite for 655.4 ms with 0.21 ms of CPU and zero runqueue
// wait, beside a heavy build, and passed three runs in three alone: a thread waiting for the disk
// is not runnable, so the third clock never moved. The fourth clock, `blocked_on_host_ns()`,
// samples the case's own scheduler state and counts the time it spends in an uninterruptible wait.
//
// The fifth close refuted the first use of it — every uninterruptible wait was excused, the case's
// own included — and the owner's rule replaced it: the guard excuses `host_stall_allowance`, at
// most the case's uninterruptible time AND at most the I/O pressure the rest of the host shows. The
// sixth close refuted THAT with a case whose own threads made the pressure — sixteen of them
// reading the disk while its vfork child held it — and the rule is now said exactly: only pressure
// from OUTSIDE the case's own process tree is excused, and `tree_blocked_ns()` is the census that
// takes the tree's own waiting out of it. The cases below are the empirical half of that rule, in
// every direction:
//
//  1. a case blocked by its OWN vfork child, its own uncached reads or its own page faults IS
//     blocked — the clock sees it — but on a host nobody else is loading it is excused nothing and
//     is a stall. The gate's own probe, run through the real guard, says the same;
//  2. a case held by its own vfork child while its OWN THREADS press the disk is a stall, on a
//     quiet host or a busy one: the census sees the helpers, and their pressure is the case's;
//  3. a case that sleeps, holds a mutex or spins accumulates no blocking and is a stall, and a held
//     mutex stays a stall even while other processes are pressing the disk hard;
//  4. a case whose disk wait sat behind OTHER processes writing and fsyncing is excused, and the
//     same writers as the case's OWN child processes excuse it nothing.
//
// THE DISK WAIT IS MADE WITH O_DIRECT, which is the one way a test can wait for the device without
// needing root to drop the page cache: every read goes to the device and the thread waits for it in
// `io_schedule()`, uninterruptibly, exactly as a major page fault does. A window runs for a fixed
// wall-clock time rather than a fixed number of reads, so a fast device and a slow one produce the
// same length of evidence. A filesystem that refuses O_DIRECT (tmpfs) reports that rather than
// asserting on a condition it could not create.
//
// THE CEILINGS ARE FRACTIONS OF THE WINDOW, so the wall clock is always over them and the verdict
// turns on the host's share alone. "Excused" is asserted at three quarters of the window — the
// host must account for a quarter, the low bar the runqueue case above uses — and a self-blocked
// case's "stall" at a SIXTEENTH of it. That is not a looser check but the rule itself: other
// processes' I/O pressure IS excusable, a case's own uncached read is slowed by it like anyone
// else's, and this machine runs builds beside its tests. For a case that blocked itself to be
// excused here, the rest of the host would have to be stalled on I/O for fifteen sixteenths of the
// window; measured beside three other agents' builds, it was at most about half.

namespace {

#if defined(__linux__)
constexpr std::size_t kBlock = 4096;
constexpr std::size_t kFileBytes = std::size_t{4} << 20U;
constexpr std::size_t kWriteChunk = std::size_t{1} << 20U;
constexpr auto kWindow = std::chrono::milliseconds(300);

std::uint64_t thread_cpu_ns() {
    timespec now{};
    ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return (static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL) +
           static_cast<std::uint64_t>(now.tv_nsec);
}

/// A 4 MiB file on the test's own filesystem, written with O_DIRECT so that every block goes to the
/// device and none is left in the page cache, then reopened with O_DIRECT for reading. Owns the
/// descriptor's lifetime and the aligned read buffer.
///
/// NOT `write` + `fsync`, which the first version did: on ext4 an `fsync` commits the journal, and
/// under a build's writeback that waited ten seconds and more for other processes' data. Writing
/// around the page cache waits for this file's four megabytes and nothing else.
class DirectFile {
public:
    explicit DirectFile(const cy::test::TempDir& directory) : path_(directory.file("direct.bin")) {
        buffer_ = static_cast<unsigned char*>(std::aligned_alloc(kBlock, kBlock));
        auto* chunk = static_cast<unsigned char*>(std::aligned_alloc(kBlock, kWriteChunk));
        const int writer =
            ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT | O_CLOEXEC, 0600);
        bool written = writer >= 0 && buffer_ != nullptr && chunk != nullptr;
        for (std::size_t offset = 0; offset < kFileBytes && written; offset += kWriteChunk) {
            std::fill(chunk, chunk + kWriteChunk, static_cast<unsigned char>(offset >> 20U));
            written = ::write(writer, chunk, kWriteChunk) == static_cast<::ssize_t>(kWriteChunk);
        }
        if (writer >= 0) {
            ::close(writer);
        }
        std::free(chunk);
        if (written) {
            descriptor_ = ::open(path_.c_str(), O_RDONLY | O_DIRECT | O_CLOEXEC);
        }
    }
    ~DirectFile() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
        std::free(buffer_);
    }
    DirectFile(const DirectFile&) = delete;
    DirectFile& operator=(const DirectFile&) = delete;

    /// False when the filesystem refused O_DIRECT or the file could not be written.
    [[nodiscard]] bool valid() const noexcept { return descriptor_ >= 0; }

    /// The file's own path, for a second, cached, mapping of it.
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    /// One uncached 4 KiB read from a scattered offset. True when the device returned the block.
    bool read_one(std::uint64_t& state) {
        state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
        const auto block = static_cast<::off_t>((state >> 33U) % (kFileBytes / kBlock));
        return ::pread(descriptor_, buffer_, kBlock, block * static_cast<::off_t>(kBlock)) ==
               static_cast<::ssize_t>(kBlock);
    }

private:
    std::string path_;
    unsigned char* buffer_ = nullptr;
    int descriptor_ = -1;
};

/// What one window cost, split the way the guard splits it.
struct Window {
    unsigned long long wall_ns = 0;
    unsigned long long cpu_ns = 0;
    unsigned long long contended_ns = 0;
    unsigned long long blocked_ns = 0;
    /// The uninterruptible time of the case's OTHER threads and its children, summed over them.
    unsigned long long tree_blocked_ns = 0;
    /// The rise in the host's I/O pressure over the window, and what of `blocked_ns` it excuses.
    unsigned long long pressure_ns = 0;
    unsigned long long allowance_ns = 0;
    unsigned long long reads = 0;
};

template <typename Body>
Window measure_window(Body&& body) {
    Window window;
    const cy::test::HostPressure pressure_before = cy::test::host_pressure_now();
    const unsigned long long contended_before = cy::test::contended_ns();
    const unsigned long long blocked_before = cy::test::blocked_on_host_ns();
    const unsigned long long tree_before = cy::test::tree_blocked_ns();
    const std::uint64_t cpu_before = thread_cpu_ns();
    const auto started = std::chrono::steady_clock::now();
    window.reads = body(started + kWindow);
    window.wall_ns = static_cast<unsigned long long>(
        std::chrono::nanoseconds(std::chrono::steady_clock::now() - started).count());
    window.cpu_ns = thread_cpu_ns() - cpu_before;
    window.contended_ns = cy::test::contended_ns() - contended_before;
    window.blocked_ns = cy::test::blocked_on_host_ns() - blocked_before;
    window.tree_blocked_ns = cy::test::tree_blocked_ns() - tree_before;
    const cy::test::HostPressure pressure_after = cy::test::host_pressure_now();
    if (pressure_before.available && pressure_after.available &&
        pressure_after.io_some_ns >= pressure_before.io_some_ns) {
        window.pressure_ns = pressure_after.io_some_ns - pressure_before.io_some_ns;
    }
    window.allowance_ns = cy::test::host_stall_allowance(
        window.wall_ns, window.blocked_ns, window.tree_blocked_ns, pressure_before, pressure_after);
    return window;
}

unsigned long long read_until(DirectFile& file, std::chrono::steady_clock::time_point deadline) {
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    unsigned long long reads = 0;
    while (std::chrono::steady_clock::now() < deadline && file.read_one(state)) {
        ++reads;
    }
    return reads;
}

/// `read_until` for a thread that shares the file with others: its own descriptor, its own
/// aligned buffer and its own scatter sequence, so sixteen of them are sixteen readers.
unsigned long long read_shared_until(const std::string& path,
                                     std::chrono::steady_clock::time_point deadline,
                                     unsigned seed) {
    auto* block = static_cast<unsigned char*>(std::aligned_alloc(kBlock, kBlock));
    const int reader = ::open(path.c_str(), O_RDONLY | O_DIRECT | O_CLOEXEC);
    unsigned long long reads = 0;
    std::uint64_t state = 0x9E3779B97F4A7C15ULL + seed;
    while (block != nullptr && reader >= 0 && std::chrono::steady_clock::now() < deadline) {
        state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
        const auto offset = static_cast<::off_t>((state >> 33U) % (kFileBytes / kBlock));
        if (::pread(reader, block, kBlock, offset * static_cast<::off_t>(kBlock)) !=
            static_cast<::ssize_t>(kBlock)) {
            break;
        }
        ++reads;
    }
    if (reader >= 0) {
        ::close(reader);
    }
    std::free(block);
    return reads;
}

/// Major page faults this thread has taken, cumulative.
unsigned long long major_faults() {
    rusage usage{};
    ::getrusage(RUSAGE_THREAD, &usage);
    return static_cast<unsigned long long>(usage.ru_majflt);
}

/// Touch a mapping of `path` one page at a time until `deadline`, evicting it from the page cache
/// before each pass so that every touch is a MAJOR fault: the page is not in memory, and the thread
/// waits in the kernel while the device reads it. `POSIX_FADV_DONTNEED` needs no privilege for a
/// file the process owns, but it skips pages that are still mapped, so each pass unmaps first.
/// `MADV_RANDOM` turns off readahead, so one fault reads one page rather than a run of them.
unsigned long long fault_until(const std::string& path,
                               std::chrono::steady_clock::time_point deadline) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return 0;
    }
    const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    unsigned long long touched = 0;
    volatile unsigned char sink = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)::posix_fadvise(descriptor, 0, 0, POSIX_FADV_DONTNEED);
        void* mapping = ::mmap(nullptr, kFileBytes, PROT_READ, MAP_PRIVATE, descriptor, 0);
        if (mapping == MAP_FAILED) {
            break;
        }
        (void)::madvise(mapping, kFileBytes, MADV_RANDOM);
        const auto* bytes = static_cast<const unsigned char*>(mapping);
        // A stride of 97 pages visits every page of the 1024 exactly once in a scattered order, so
        // neither the device nor the kernel sees a sequential run it could prefetch.
        const std::size_t pages = kFileBytes / page;
        for (std::size_t visit = 0; visit < pages && std::chrono::steady_clock::now() < deadline;
             ++visit) {
            sink = sink + bytes[((visit * 97U) % pages) * page];
            ++touched;
        }
        ::munmap(mapping, kFileBytes);
    }
    ::close(descriptor);
    (void)sink;
    return touched;
}

/// Wait on a mutex another thread holds until `deadline`: a futex, an interruptible sleep.
unsigned long long wait_on_held_mutex(std::chrono::steady_clock::time_point deadline) {
    std::mutex held;
    std::mutex handshake;
    std::condition_variable locked;
    bool is_locked = false;
    std::thread holder([&]() {
        const std::lock_guard<std::mutex> hold(held);
        {
            const std::lock_guard<std::mutex> tell(handshake);
            is_locked = true;
        }
        locked.notify_one();
        std::this_thread::sleep_until(deadline);
    });
    {
        std::unique_lock<std::mutex> wait(handshake);
        locked.wait(wait, [&]() { return is_locked; });
    }
    {
        const std::lock_guard<std::mutex> take(held);
    }
    holder.join();
    return 1;
}

/// Block in the parent of a `vfork` until `deadline`: an UNINTERRUPTIBLE wait the case causes
/// itself, with nothing else on the host involved. The child calls only async-signal-safe
/// functions.
unsigned long long vfork_until(std::chrono::steady_clock::time_point deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
        deadline - std::chrono::steady_clock::now());
    const long long total = std::max<long long>(remaining.count(), 0);
    timespec held{static_cast<time_t>(total / 1'000'000'000LL),
                  static_cast<long>(total % 1'000'000'000LL)};
    // vfork IS the subject: its parent's uninterruptible wait is what is being measured, and
    // posix_spawn, which the check suggests, would hide it.
    // NOLINTNEXTLINE(bugprone-unsafe-functions,clang-analyzer-security.insecureAPI.vfork)
    const pid_t child = ::vfork();
    if (child == 0) {
        // NOLINTNEXTLINE(clang-analyzer-unix.Vfork): nanosleep is async-signal-safe
        while (::nanosleep(&held, &held) != 0) {
        }
        ::_exit(0);
    }
    if (child < 0) {
        return 0;
    }
    int status = 0;
    (void)::waitpid(child, &status, 0);
    return 1;
}

void report(const char* what, const Window& window) {
    CY_TEST_MESSAGE(std::string(what)
                    << ": " << (static_cast<double>(window.wall_ns) / 1e6) << " ms wall, "
                    << (static_cast<double>(window.cpu_ns) / 1e6) << " ms CPU, "
                    << (static_cast<double>(window.contended_ns) / 1e6) << " ms runqueue, "
                    << (static_cast<double>(window.blocked_ns) / 1e6) << " ms uninterruptible, "
                    << (static_cast<double>(window.tree_blocked_ns) / 1e6)
                    << " ms uninterruptible in its other threads and children, "
                    << (static_cast<double>(window.pressure_ns) / 1e6) << " ms host I/O pressure, "
                    << (static_cast<double>(window.allowance_ns) / 1e6) << " ms excused, "
                    << window.reads << " reads or touches");
}

/// The verdict the guard would reach on this window against a ceiling of `parts / whole` of it.
cy::test::StallVerdict verdict_at(const Window& window, unsigned long long parts,
                                  unsigned long long whole) {
    return cy::test::stall_verdict(window.wall_ns, window.contended_ns + window.allowance_ns,
                                   (window.wall_ns / whole) * parts);
}

/// Assert what every self-blocked window must show: the clock SAW the wait, the allowance is
/// bounded by both the wait and the host's pressure, and the case is a stall.
void expect_blocked_but_not_excused(const Window& window) {
    CY_CHECK_GT(window.blocked_ns, window.wall_ns / 4U);
    CY_CHECK_LE(window.allowance_ns, window.blocked_ns);
    CY_CHECK_LE(window.allowance_ns, window.pressure_ns);
    CY_CHECK(verdict_at(window, 1U, 16U) == cy::test::StallVerdict::Stalled);
}
#endif

}  // namespace

CY_TEST_CASE("harness: a case blocked by its OWN vfork child is blocked, and still a stall") {
    if (!cy::test::budget_measures_host_blocking()) {
        CY_TEST_MESSAGE(
            "this platform does not report host blocking; the stall ceiling is unchanged");
        return;
    }
#if defined(__linux__)
    // THE GATE'S PROBE, IN THE GUARD'S ARITHMETIC. The parent of a vfork waits uninterruptibly, so
    // the fourth clock sees almost the whole window — which is exactly why subtracting that clock
    // was wrong. The host's I/O pressure does not move for it, so nothing is excused.
    const Window window = measure_window(
        [](std::chrono::steady_clock::time_point deadline) { return vfork_until(deadline); });
    report("own vfork child", window);
    CY_REQUIRE(window.reads == 1U);
    CY_REQUIRE(window.wall_ns >= 250'000'000ULL);
    CY_CHECK_GT(window.blocked_ns, window.wall_ns / 2U);
    expect_blocked_but_not_excused(window);
#endif
}

CY_TEST_CASE(
    "harness: a case waiting on its OWN uncached reads is blocked, and on a quiet host a stall") {
    if (!cy::test::budget_measures_host_blocking()) {
        CY_TEST_MESSAGE(
            "this platform does not report host blocking; the stall ceiling is unchanged");
        return;
    }
#if defined(__linux__)
    // The case's own I/O raises the host's pressure too, by at most its own wait weighted by its
    // processor's share of the machine; `host_stall_allowance` takes that out before excusing.
    cy::test::TempDir directory{"budget-disk-wait"};
    CY_REQUIRE(directory.valid());
    DirectFile file{directory};
    if (!file.valid()) {
        CY_TEST_MESSAGE(
            "this filesystem refuses O_DIRECT; an uncached wait cannot be created here");
        return;
    }

    const Window window = measure_window(
        [&](std::chrono::steady_clock::time_point deadline) { return read_until(file, deadline); });
    report("own disk wait", window);
    CY_REQUIRE(window.reads > 0U);
    CY_REQUIRE(window.wall_ns >= 250'000'000ULL);
    expect_blocked_but_not_excused(window);
#endif
}

CY_TEST_CASE("harness: a case waiting on its OWN major page faults is blocked, and a stall") {
    if (!cy::test::budget_measures_host_blocking()) {
        CY_TEST_MESSAGE(
            "this platform does not report host blocking; the stall ceiling is unchanged");
        return;
    }
#if defined(__linux__)
    // THE FOURTH CLOSE'S MECHANISM. A case that does nothing but run its own code for the first
    // time takes a major fault for every page a build has pushed out of the page cache. This makes
    // that happen on purpose, through a mapping, with nothing else loading the disk: the clock sees
    // it, and it is the case's own.
    cy::test::TempDir directory{"budget-page-faults"};
    CY_REQUIRE(directory.valid());
    DirectFile file{directory};
    if (!file.valid()) {
        CY_TEST_MESSAGE("this filesystem refuses O_DIRECT; the file could not be prepared here");
        return;
    }

    const unsigned long long faults_before = major_faults();
    const Window window = measure_window([&](std::chrono::steady_clock::time_point deadline) {
        return fault_until(file.path(), deadline);
    });
    const unsigned long long faults = major_faults() - faults_before;
    report("own major page faults", window);
    CY_TEST_MESSAGE(faults << " major faults");
    CY_REQUIRE(window.wall_ns >= 250'000'000ULL);
    // The premise: the pages really were not in memory. A kernel that ignored the eviction would
    // serve every touch from the cache, and there would be nothing for the clock to see.
    CY_REQUIRE(faults > 0U);
    expect_blocked_but_not_excused(window);
#endif
}

CY_TEST_CASE(
    "harness: a case held by its OWN vfork child while its OWN threads read the disk is a stall") {
    if (!cy::test::budget_measures_host_blocking()) {
        CY_TEST_MESSAGE(
            "this platform does not report host blocking; the stall ceiling is unchanged");
        return;
    }
#if defined(__linux__)
    // THE SIXTH CLOSE'S PROBE, IN THE GUARD'S ARITHMETIC. The case's thread waits uninterruptibly
    // on its vfork child for the whole window, and sixteen of its own threads keep the device busy
    // with uncached reads for the same window, so the host's I/O pressure IS high — and every bit
    // of it is the case's own. The census counts the helpers, and the allowance is what is left of
    // the pressure after their share and the case's are taken out, which on a quiet host is
    // nothing and on a busy one is only what other processes made.
    cy::test::TempDir directory{"budget-own-helpers"};
    CY_REQUIRE(directory.valid());
    DirectFile file{directory};
    if (!file.valid()) {
        CY_TEST_MESSAGE("this filesystem refuses O_DIRECT; the helpers cannot press the disk here");
        return;
    }
    if (!cy::test::budget_measures_tree_blocking()) {
        // No census, no allowance: this is the rule without its instrument, and the case is still
        // a stall — through zero rather than through the subtraction.
        CY_TEST_MESSAGE(
            "this kernel has no /proc/<pid>/task/<tid>/children; the allowance must "
            "be zero");
    }

    constexpr unsigned kHelpers = 16;
    std::atomic<unsigned long long> helper_reads{0};
    const Window window = measure_window([&](std::chrono::steady_clock::time_point deadline) {
        // Every helper reads the one file the case wrote, through a descriptor and buffer of its
        // own: O_DIRECT sends each read to the device whether or not another thread just read
        // the same block.
        std::vector<std::thread> helpers;
        helpers.reserve(kHelpers);
        for (unsigned index = 0; index < kHelpers; ++index) {
            helpers.emplace_back(
                [&, index]() { helper_reads += read_shared_until(file.path(), deadline, index); });
        }
        const unsigned long long held = vfork_until(deadline);
        for (std::thread& helper : helpers) {
            helper.join();
        }
        return held;
    });
    report("own vfork child beside the case's own sixteen disk readers", window);
    CY_TEST_MESSAGE(helper_reads.load() << " reads by the helpers");
    CY_REQUIRE(window.reads == 1U);
    CY_REQUIRE(helper_reads.load() > 0U);  // the premise: the helpers really pressed the disk
    CY_REQUIRE(window.wall_ns >= 250'000'000ULL);
    CY_CHECK_GT(window.blocked_ns, window.wall_ns / 2U);
    if (cy::test::budget_measures_tree_blocking()) {
        // Sixteen readers blocked for most of the window are several windows between them; the
        // census cannot have missed them.
        CY_CHECK_GT(window.tree_blocked_ns, 4U * window.wall_ns);
    } else {
        CY_CHECK_EQ(window.allowance_ns, 0ULL);
    }
    expect_blocked_but_not_excused(window);
#endif
}

CY_TEST_CASE("harness: a case that sleeps while the disk is busy is still a stall") {
    if (!cy::test::budget_measures_host_blocking()) {
        CY_TEST_MESSAGE(
            "this platform does not report host blocking; the stall ceiling is unchanged");
        return;
    }
#if defined(__linux__)
    // Another thread of this very process keeps the same device busy with uncached reads for the
    // whole window, so the host IS loaded with I/O while the case itself only sleeps. The case did
    // not wait uninterruptibly, so the allowance — never more than that — is nothing.
    cy::test::TempDir directory{"budget-sleep-beside-disk"};
    CY_REQUIRE(directory.valid());
    DirectFile file{directory};
    if (!file.valid()) {
        CY_TEST_MESSAGE("this filesystem refuses O_DIRECT; a busy disk cannot be created here");
        return;
    }

    std::atomic<unsigned long long> background_reads{0};
    const Window window = measure_window([&](std::chrono::steady_clock::time_point deadline) {
        std::thread loader([&]() { background_reads = read_until(file, deadline); });
        std::this_thread::sleep_until(deadline);
        loader.join();
        return background_reads.load();
    });
    report("sleep beside a busy disk", window);
    CY_REQUIRE(window.reads > 0U);  // the premise: the disk really was busy during the window
    CY_REQUIRE(window.wall_ns >= 250'000'000ULL);

    CY_CHECK_LT(window.blocked_ns, window.wall_ns / 10U);
    CY_CHECK_LE(window.allowance_ns, window.blocked_ns);
    CY_CHECK(verdict_at(window, 3U, 4U) == cy::test::StallVerdict::Stalled);
#endif
}

CY_TEST_CASE("harness: a case that burns its own CPU is not blocked by the host") {
    if (!cy::test::budget_measures_host_blocking()) {
        CY_TEST_MESSAGE(
            "this platform does not report host blocking; the stall ceiling is unchanged");
        return;
    }
#if defined(__linux__)
    // Running and runnable are not blocked. A case that spins is charged its CPU by the budget and
    // its preemption by the runqueue clock; the fourth clock must add nothing to either, or a case
    // that is merely slow would be excused as a case that was merely waiting.
    const Window window = measure_window([&](std::chrono::steady_clock::time_point deadline) {
        volatile unsigned long long sink = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            sink = sink + 1U;
        }
        return 0ULL;
    });
    report("busy case", window);
    CY_REQUIRE(window.wall_ns >= 250'000'000ULL);

    CY_CHECK_LT(window.blocked_ns, window.wall_ns / 10U);
    CY_CHECK_LE(window.allowance_ns, window.blocked_ns);
    // With the runqueue left out, the host's share alone excuses nothing here.
    CY_CHECK(
        cy::test::stall_verdict(window.wall_ns, window.allowance_ns, (window.wall_ns / 4U) * 3U) ==
        cy::test::StallVerdict::Stalled);
#endif
}

// --- THE GATE'S PROBE, THROUGH THE REAL GUARD ------------------------------------------------
//
// Every case above re-enacts the guard's arithmetic on a window this file measured. These run
// stall_probe.cpp — real `CY_TEST_CASE`s under the unit tier's budget — as a child process and read
// the verdict the guard itself printed. CY_TEST_BUDGET_SCALE=0.25 makes the budget 0.25 ms and the
// ceiling exactly 25 ms, so each 300 ms case is twelve times over it: for the vfork case to be
// excused, other processes would have to show 275 ms of I/O pressure in the 280 ms after the
// guard's 20 ms pressure baseline. `757b3d9` excused it with no pressure at all.

namespace {

#if defined(__linux__)
/// What a probe run printed, and how it ended.
struct ProbeRun {
    bool started = false;
    int status = 0;
    std::string output;
};

ProbeRun run_probe(const char* test_case, const char* scale) {
    ProbeRun run;
    int pipe_ends[2];
    if (::pipe(pipe_ends) != 0) {
        return run;
    }
    // The child's environment is built here rather than by `setenv`, which would change the scale
    // this process's own guard re-reads.
    std::vector<std::string> variables;
    for (char** entry = environ; *entry != nullptr; ++entry) {
        if (std::strncmp(*entry, "CY_TEST_BUDGET_SCALE=", 21) != 0) {
            variables.emplace_back(*entry);
        }
    }
    variables.emplace_back(std::string("CY_TEST_BUDGET_SCALE=") + scale);
    std::vector<char*> envp;
    envp.reserve(variables.size() + 1);
    for (std::string& variable : variables) {
        envp.push_back(variable.data());
    }
    envp.push_back(nullptr);
    std::string program = CY_STALL_PROBE;
    std::string filter = std::string("--test-case=") + test_case;
    char* argv[] = {program.data(), filter.data(), nullptr};

    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDOUT_FILENO);
    ::posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDERR_FILENO);
    ::posix_spawn_file_actions_addclose(&actions, pipe_ends[0]);
    pid_t child = 0;
    const int spawned =
        ::posix_spawn(&child, program.c_str(), &actions, nullptr, argv, envp.data());
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(pipe_ends[1]);
    if (spawned == 0) {
        char buffer[4096];
        ::ssize_t got = 0;
        while ((got = ::read(pipe_ends[0], buffer, sizeof(buffer))) > 0) {
            run.output.append(buffer, static_cast<std::size_t>(got));
        }
        run.started = ::waitpid(child, &run.status, 0) == child;
    }
    ::close(pipe_ends[0]);
    return run;
}

/// The probe failed the case, and said why with `verdict` — and never called it contended.
void expect_probe_failed(const char* test_case, const char* verdict, const char* scale = "0.25") {
    const ProbeRun run = run_probe(test_case, scale);
    CY_TEST_MESSAGE(test_case << ":\n" << run.output);
    CY_REQUIRE(run.started);
    CY_CHECK(WIFEXITED(run.status));
    CY_CHECK_NE(WEXITSTATUS(run.status), 0);
    CY_CHECK(run.output.find(verdict) != std::string::npos);
    CY_CHECK(run.output.find("contended:") == std::string::npos);
}
#endif

}  // namespace

CY_TEST_CASE("harness: the gate's probe — a case held by its own vfork child fails as stalled") {
#if defined(__linux__)
    // M11.c's fifth close: `757b3d9` reported this case as "contended: ... 300.040 ms blocked on
    // the host's disk or a page fault" and passed it. It must fail.
    expect_probe_failed("probe: a case whose own vfork child holds it", "stalled:");
#else
    CY_TEST_MESSAGE("the probe's vfork case is Linux's; nothing to run here");
#endif
}

CY_TEST_CASE(
    "harness: the gate's probe — a case held by its own vfork child while its own threads read "
    "the disk fails as stalled") {
#if defined(__linux__)
    // M11.c's sixth close: `4a1ad21` reported this case as "contended: ... 212.557 ms ... was the
    // host's I/O pressure" and passed it, because the helpers' pressure was subtracted as the
    // calling thread's alone. It must fail, whatever the rest of the host does.
    //
    // THE CEILING IS 100 ms HERE, a third of the window, which is the gate's own arithmetic: at
    // scale 1 the unit budget is 1 ms, the case's thread spends about half of that creating the
    // foreman and waiting on it, and the case is excused only if the host accounts for two thirds
    // of its wait. `4a1ad21` excused 282 ms of the 300 on a quiet host (sixteen readers and the
    // case are the whole of the non-idle time, so the pressure is the whole window and the
    // calling thread's share of it 18 ms) and about 200 on a host with every processor busy. The
    // census leaves at most a third excusable in either state.
    expect_probe_failed(
        "probe: a case whose own vfork child holds it while its own threads read the disk",
        "stalled:", "1");
#else
    CY_TEST_MESSAGE("the probe's vfork case is Linux's; nothing to run here");
#endif
}

CY_TEST_CASE("harness: the gate's probe — a held mutex fails as stalled, a spin as over budget") {
#if defined(__linux__)
    expect_probe_failed("probe: a case waiting on a mutex another thread holds", "stalled:");
    expect_probe_failed("probe: a case that spins", "over budget:");
#else
    CY_TEST_MESSAGE("the probe is run as a POSIX child process; nothing to run here");
#endif
}

// --- DIRECTION 4: OTHER PROCESSES PRESSING THE DISK ----------------------------------------------
//
// The allowance exists for this: a case whose own small wait for the device sat behind somebody
// else's I/O. The somebody else is made here as SEPARATE PROCESSES, one pinned to each processor
// this test may use, each writing a megabyte at a time and fsyncing every eight — PSI counts a
// processor as stalled while any task queued on it is, and averages over processors, so pressure
// the host shows has to be pressure on many processors at once. Measured on this project's host
// with every processor already compiling: 200 to 270 ms of the 300 ms window. Each writer is
// bounded three ways — killed by PID when the case ends, an `alarm` it set itself, and the case's
// own wall-clock ceiling — and writes one 8 MiB file in place, so nothing grows.
//
// SEPARATE MEANS OUTSIDE THE CASE'S PROCESS TREE, which M11.c's sixth close made exact: a writer
// this process forked is the case's own child, the census counts it, and its pressure is the
// case's. So the writers are ORPHANED — forked by a child that exits at once, which re-parents
// them to init (or the nearest subreaper) — and are then what a build in another terminal is. The
// same writers left as the case's own children are the sixth close's other direction, and the
// second case below asserts that they excuse nothing.

namespace {

#if defined(__linux__)
constexpr std::size_t kWriterChunks = 8;
constexpr unsigned kWriterLifetimeSeconds = 20;
constexpr auto kWriterWarmup = std::chrono::milliseconds(500);

/// Only system calls after the fork: this process has threads, and one of them may hold a lock the
/// child would inherit held.
[[noreturn]] void write_and_fsync_forever(const char* path, const unsigned char* chunk, int cpu) {
    ::alarm(kWriterLifetimeSeconds);
    cpu_set_t only;
    CPU_ZERO(&only);
    CPU_SET(cpu, &only);
    (void)::sched_setaffinity(0, sizeof(only), &only);
    const int descriptor = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        ::_exit(1);
    }
    for (;;) {
        for (std::size_t index = 0; index < kWriterChunks; ++index) {
            if (::pwrite(descriptor, chunk, kWriteChunk,
                         static_cast<::off_t>(index * kWriteChunk)) < 0) {
                ::_exit(1);  // a full disk ends the writer, and the premise check sees it
            }
        }
        (void)::fsync(descriptor);
    }
}

/// Whose processes the writers are.
enum class Parentage {
    /// Forked by this process: the case's own children, inside its tree.
    Children,
    /// Forked by a short-lived child of this process and re-parented away from it when that child
    /// exits: outside the case's tree, as a build in another terminal is.
    Orphaned,
};

/// The writer processes, alive for this object's lifetime.
class ExternalWriters {
public:
    ExternalWriters(const cy::test::TempDir& directory, Parentage parentage)
        : parentage_(parentage), chunk_(kWriteChunk, static_cast<unsigned char>(0x5A)) {
        cpu_set_t allowed;
        CPU_ZERO(&allowed);
        if (::sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
            return;
        }
        std::vector<int> cpus;
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (CPU_ISSET(cpu, &allowed)) {
                cpus.push_back(cpu);
            }
        }
        for (const int cpu : cpus) {
            paths_.push_back(directory.file(("writer-" + std::to_string(cpu) + ".bin").c_str()));
        }
        if (parentage == Parentage::Children) {
            for (std::size_t index = 0; index < cpus.size(); ++index) {
                const pid_t child = ::fork();
                if (child == 0) {
                    write_and_fsync_forever(paths_[index].c_str(), chunk_.data(), cpus[index]);
                }
                if (child > 0) {
                    writers_.push_back(child);
                }
            }
            return;
        }
        spawn_orphaned(cpus);
    }
    ~ExternalWriters() {
        for (const pid_t writer : writers_) {
            (void)::kill(writer, SIGKILL);
        }
        for (const pid_t writer : writers_) {
            if (parentage_ == Parentage::Children) {
                int status = 0;
                (void)::waitpid(writer, &status, 0);
            } else {
                // Not this process's to reap: init does. Gone once the kernel no longer knows it.
                while (::kill(writer, 0) == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }
    }
    ExternalWriters(const ExternalWriters&) = delete;
    ExternalWriters& operator=(const ExternalWriters&) = delete;

    [[nodiscard]] std::size_t count() const noexcept { return writers_.size(); }

    /// True while every writer is still running: none failed to open its file, none has exited.
    [[nodiscard]] bool all_running() const noexcept {
        return std::ranges::all_of(writers_, [this](pid_t writer) {
            if (parentage_ == Parentage::Orphaned) {
                return ::kill(writer, 0) == 0;
            }
            int status = 0;
            return ::waitpid(writer, &status, WNOHANG) == 0;
        });
    }

private:
    /// A child forks one writer per processor, writes their pids down a pipe and exits, so that
    /// every writer is re-parented away from this process before this returns.
    void spawn_orphaned(const std::vector<int>& cpus) {
        int pipe_ends[2];
        if (::pipe(pipe_ends) != 0) {
            return;
        }
        const pid_t intermediary = ::fork();
        if (intermediary == 0) {
            ::close(pipe_ends[0]);
            for (std::size_t index = 0; index < cpus.size(); ++index) {
                const pid_t writer = ::fork();
                if (writer == 0) {
                    // The writer must not hold the pipe's write end: the parent reads pids until
                    // EOF, and a writer that kept it open would hold the parent until its own
                    // `alarm` ended it — twenty seconds later, with every writer dead.
                    ::close(pipe_ends[1]);
                    write_and_fsync_forever(paths_[index].c_str(), chunk_.data(), cpus[index]);
                }
                if (writer > 0) {
                    (void)::write(pipe_ends[1], &writer, sizeof(writer));
                }
            }
            ::_exit(0);
        }
        ::close(pipe_ends[1]);
        if (intermediary > 0) {
            pid_t writer = 0;
            while (::read(pipe_ends[0], &writer, sizeof(writer)) == sizeof(writer)) {
                writers_.push_back(writer);
            }
            int status = 0;
            (void)::waitpid(intermediary, &status, 0);
        }
        ::close(pipe_ends[0]);
    }

    Parentage parentage_;
    std::vector<unsigned char> chunk_;
    std::vector<std::string> paths_;
    std::vector<pid_t> writers_;
};

/// Runs the disk-wait and held-mutex windows beside writers of the given parentage, reporting
/// both. Nothing is measured when the writers could not be made.
struct BesideWriters {
    Window disk;
    Window mutex;
    bool measured = false;
};

BesideWriters measure_beside_writers(const cy::test::TempDir& directory, DirectFile& file,
                                     Parentage parentage, const char* who) {
    BesideWriters beside;
    const ExternalWriters writers{directory, parentage};
    CY_REQUIRE(writers.count() > 0U);
    std::this_thread::sleep_for(kWriterWarmup);
    CY_REQUIRE(writers.all_running());

    beside.disk = measure_window(
        [&](std::chrono::steady_clock::time_point deadline) { return read_until(file, deadline); });
    report((std::string("disk wait behind ") + who + " writes").c_str(), beside.disk);
    beside.mutex = measure_window([](std::chrono::steady_clock::time_point deadline) {
        return wait_on_held_mutex(deadline);
    });
    report((std::string("held mutex beside ") + who + " writes").c_str(), beside.mutex);
    CY_REQUIRE(writers.all_running());  // the premise held for both windows
    CY_REQUIRE(beside.disk.reads > 0U);
    CY_REQUIRE(beside.disk.wall_ns >= 250'000'000ULL);
    CY_REQUIRE(beside.mutex.wall_ns >= 250'000'000ULL);
    beside.measured = true;
    return beside;
}

/// A held mutex is an interruptible wait: however hard the host is pressed, nothing is excused.
void expect_mutex_still_a_stall(const Window& mutex) {
    CY_CHECK_LT(mutex.blocked_ns, mutex.wall_ns / 10U);
    CY_CHECK_LE(mutex.allowance_ns, mutex.blocked_ns);
    CY_CHECK(verdict_at(mutex, 3U, 4U) == cy::test::StallVerdict::Stalled);
}
#endif

}  // namespace

CY_TEST_CASE(
    "harness: a case delayed by OTHER processes' disk pressure is excused, a held mutex is not") {
    if (!cy::test::budget_measures_host_blocking()) {
        CY_TEST_MESSAGE(
            "this platform does not report host blocking; the stall ceiling is unchanged");
        return;
    }
#if defined(__linux__)
    cy::test::TempDir directory{"budget-external-pressure"};
    CY_REQUIRE(directory.valid());
    DirectFile file{directory};
    if (!file.valid()) {
        CY_TEST_MESSAGE("this filesystem refuses O_DIRECT; an uncached wait cannot be made here");
        return;
    }

    const BesideWriters beside =
        measure_beside_writers(directory, file, Parentage::Orphaned, "other processes'");
    if (!beside.measured) {
        return;
    }
    expect_mutex_still_a_stall(beside.mutex);

    const Window& disk = beside.disk;
    if (!cy::test::budget_measures_host_pressure()) {
        // THE RULE WITHOUT ITS INSTRUMENT: no pressure reading, no allowance — never an unlimited
        // one. The disk wait is then the case's, as it was before the fourth clock existed.
        CY_TEST_MESSAGE("this host has no /proc/pressure/io; the allowance must be zero");
        CY_CHECK_EQ(disk.allowance_ns, 0ULL);
        CY_CHECK(verdict_at(disk, 3U, 4U) == cy::test::StallVerdict::Stalled);
        return;
    }
    // The writers are outside the tree, so the census saw at most the odd fault of this process's
    // own threads; the case waited on the device for most of the window, the rest of the host was
    // stalled on I/O for more than a quarter of it, and the allowance — the smaller of the two —
    // carries the verdict from "the case" to "the host".
    CY_CHECK_LT(disk.tree_blocked_ns, disk.wall_ns / 10U);
    CY_CHECK_GT(disk.blocked_ns, disk.wall_ns / 4U);
    CY_CHECK_LE(disk.allowance_ns, disk.blocked_ns);
    CY_CHECK_LE(disk.allowance_ns, disk.pressure_ns);
    CY_CHECK_GT(disk.allowance_ns, disk.wall_ns / 4U);
    CY_CHECK(verdict_at(disk, 3U, 4U) == cy::test::StallVerdict::Contended);
#endif
}

CY_TEST_CASE("harness: a case delayed by its OWN child processes' disk pressure is not excused") {
    if (!cy::test::budget_measures_host_blocking()) {
        CY_TEST_MESSAGE(
            "this platform does not report host blocking; the stall ceiling is unchanged");
        return;
    }
#if defined(__linux__)
    // THE SIXTH CLOSE'S OTHER DIRECTION. The same writers, the same pressure — but forked by this
    // process, so they are the case's own children. `4a1ad21` excused this window exactly as the
    // one above; the census counts every child, and takes the whole of their pressure out.
    cy::test::TempDir directory{"budget-own-children-pressure"};
    CY_REQUIRE(directory.valid());
    DirectFile file{directory};
    if (!file.valid()) {
        CY_TEST_MESSAGE("this filesystem refuses O_DIRECT; an uncached wait cannot be made here");
        return;
    }
    if (!cy::test::budget_measures_tree_blocking()) {
        CY_TEST_MESSAGE(
            "this kernel has no /proc/<pid>/task/<tid>/children; the allowance must "
            "be zero");
    }

    const BesideWriters beside =
        measure_beside_writers(directory, file, Parentage::Children, "its own children's");
    if (!beside.measured) {
        return;
    }
    expect_mutex_still_a_stall(beside.mutex);

    const Window& disk = beside.disk;
    CY_CHECK_GT(disk.blocked_ns, disk.wall_ns / 4U);
    if (cy::test::budget_measures_tree_blocking()) {
        // A writer per processor, each stalled for most of the window: many windows between them.
        CY_CHECK_GT(disk.tree_blocked_ns, 4U * disk.wall_ns);
    } else {
        CY_CHECK_EQ(disk.allowance_ns, 0ULL);
    }
    expect_blocked_but_not_excused(disk);
#endif
}
