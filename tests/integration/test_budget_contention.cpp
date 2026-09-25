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
#include <cy/test/quiet_host.h>
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
// M11.c's FOURTH TO SEVENTH CLOSES: the disk, which the runqueue clock cannot see and the guard
// does not excuse
// ================================================================================================
//
// `unit.determinism`'s first case held the suite for 655.4 ms with 0.21 ms of CPU and zero runqueue
// wait, beside a heavy build, and passed three runs in three alone: a thread waiting for the disk
// is not runnable, so the third clock never moved. The fourth clock, `blocked_on_host_ns()`,
// samples the case's own scheduler state and counts the time it spends in an uninterruptible wait.
//
// Three closes tried to excuse some of that clock as the host's and three gates refuted them, each
// with a case that made the wait itself: its own vfork child (the fifth close, 300 ms excused), its
// own sixteen threads reading the disk beside that vfork (the sixth, 212 ms), and the same readers
// double-forked and re-parented to init, past any census of the case's tree (the seventh, 208 ms
// through the real guard). THE OWNER'S DECISION: the harness excuses NO uninterruptible wait; the
// clock is a diagnostic in the stall message; and the premise that the host is quiet is stated in
// the ledger criteria and checked from outside the process by `tools/quiet-host/`. The cases below
// are the empirical half of that decision:
//
//  1. a case blocked by its OWN vfork child, its own uncached reads or its own page faults IS
//     blocked — the clock sees it, which is what makes the stall message useful — and it is a
//     stall, on any host: the clock is not among the verdict's inputs;
//  2. a case held by its own vfork child while its OWN THREADS press the disk is a stall;
//  3. a case that sleeps, holds a mutex or spins accumulates no blocking and is a stall;
//  4. through the REAL guard, as a child process: the fifth close's probe, the sixth close's, and
//     the seventh close's — the helpers orphaned by a double fork — each fail as `stalled:`, and
//     none is ever reported `contended:`.
//
// THE DISK WAIT IS MADE WITH O_DIRECT, which is the one way a test can wait for the device without
// needing root to drop the page cache: every read goes to the device and the thread waits for it in
// `io_schedule()`, uninterruptibly, exactly as a major page fault does. A window runs for a fixed
// wall-clock time rather than a fixed number of reads, so a fast device and a slow one produce the
// same length of evidence. A filesystem that refuses O_DIRECT (tmpfs) reports that rather than
// asserting on a condition it could not create.
//
// THE CEILINGS ARE FRACTIONS OF THE WINDOW, so the wall clock is always over them and the verdict
// turns on runqueue wait alone. A self-blocked case's "stall" is asserted at a SIXTEENTH of the
// window: the case did not wait for a core, so nothing is subtracted, and the whole window is its
// own.

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
    unsigned long long reads = 0;
};

template <typename Body>
Window measure_window(Body&& body) {
    Window window;
    const unsigned long long contended_before = cy::test::contended_ns();
    const unsigned long long blocked_before = cy::test::blocked_on_host_ns();
    const std::uint64_t cpu_before = thread_cpu_ns();
    const auto started = std::chrono::steady_clock::now();
    window.reads = body(started + kWindow);
    window.wall_ns = static_cast<unsigned long long>(
        std::chrono::nanoseconds(std::chrono::steady_clock::now() - started).count());
    window.cpu_ns = thread_cpu_ns() - cpu_before;
    window.contended_ns = cy::test::contended_ns() - contended_before;
    window.blocked_ns = cy::test::blocked_on_host_ns() - blocked_before;
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
                    << window.reads << " reads or touches");
}

/// The verdict the guard would reach on this window against a ceiling of `parts / whole` of it.
cy::test::StallVerdict verdict_at(const Window& window, unsigned long long parts,
                                  unsigned long long whole) {
    return cy::test::stall_verdict(window.wall_ns, window.contended_ns,
                                   (window.wall_ns / whole) * parts);
}

/// Assert what every self-blocked window must show: the clock SAW the wait, so the stall message
/// can say so, and the case is a stall — the clock is not among the verdict's inputs.
void expect_blocked_and_stalled(const Window& window) {
    CY_CHECK_GT(window.blocked_ns, window.wall_ns / 4U);
    CY_CHECK(verdict_at(window, 1U, 16U) == cy::test::StallVerdict::Stalled);
}
#endif

}  // namespace

CY_TEST_CASE("harness: a case blocked by its OWN vfork child is blocked, and a stall") {
    if (!cy::test::budget_measures_host_blocking()) {
        CY_TEST_MESSAGE(
            "this platform does not report host blocking; the stall ceiling is unchanged");
        return;
    }
#if defined(__linux__)
    // THE FIFTH CLOSE'S PROBE, IN THE GUARD'S ARITHMETIC. The parent of a vfork waits
    // uninterruptibly, so the fourth clock sees almost the whole window — which is exactly why
    // subtracting that clock was wrong, and why it is not subtracted.
    const Window window = measure_window(
        [](std::chrono::steady_clock::time_point deadline) { return vfork_until(deadline); });
    report("own vfork child", window);
    CY_REQUIRE(window.reads == 1U);
    CY_REQUIRE(window.wall_ns >= 250'000'000ULL);
    CY_CHECK_GT(window.blocked_ns, window.wall_ns / 2U);
    expect_blocked_and_stalled(window);
#endif
}

CY_TEST_CASE("harness: a case waiting on its OWN uncached reads is blocked, and a stall") {
    if (!cy::test::budget_measures_host_blocking()) {
        CY_TEST_MESSAGE(
            "this platform does not report host blocking; the stall ceiling is unchanged");
        return;
    }
#if defined(__linux__)
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
    expect_blocked_and_stalled(window);
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
    // that happen on purpose, through a mapping: the clock sees it, so the stall message can name
    // it, and the verdict is a stall — the quiet host the criteria assume is where that verdict is
    // trusted, and a build beside the run is what `tools/quiet-host/` refuses.
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
    expect_blocked_and_stalled(window);
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
    // on its vfork child for the whole window while sixteen of its own threads keep the device
    // busy with uncached reads, so the host's I/O pressure IS high and every bit of it is the
    // case's own. No census is needed to say so: pressure is not an input the verdict takes.
    cy::test::TempDir directory{"budget-own-helpers"};
    CY_REQUIRE(directory.valid());
    DirectFile file{directory};
    if (!file.valid()) {
        CY_TEST_MESSAGE("this filesystem refuses O_DIRECT; the helpers cannot press the disk here");
        return;
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
    expect_blocked_and_stalled(window);
#endif
}

CY_TEST_CASE("harness: a case that sleeps while the disk is busy is not blocked, and a stall") {
    if (!cy::test::budget_measures_host_blocking()) {
        CY_TEST_MESSAGE(
            "this platform does not report host blocking; the stall ceiling is unchanged");
        return;
    }
#if defined(__linux__)
    // Another thread of this very process keeps the same device busy with uncached reads for the
    // whole window, so the host IS loaded with I/O while the case itself only sleeps. The clock
    // must not count the sleep, or the stall message would send a reader to the disk.
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
    CY_CHECK(verdict_at(window, 3U, 4U) == cy::test::StallVerdict::Stalled);
#endif
}

CY_TEST_CASE(
    "harness: a case waiting on a mutex another thread holds is not blocked, and a stall") {
    if (!cy::test::budget_measures_host_blocking()) {
        CY_TEST_MESSAGE(
            "this platform does not report host blocking; the stall ceiling is unchanged");
        return;
    }
#if defined(__linux__)
    // A futex wait is an interruptible sleep: the clock reads `S`, so the stall message says the
    // case was not waiting for the disk, and the verdict is a stall either way.
    const Window window = measure_window([](std::chrono::steady_clock::time_point deadline) {
        return wait_on_held_mutex(deadline);
    });
    report("held mutex", window);
    CY_REQUIRE(window.wall_ns >= 250'000'000ULL);
    CY_CHECK_LT(window.blocked_ns, window.wall_ns / 10U);
    CY_CHECK(verdict_at(window, 3U, 4U) == cy::test::StallVerdict::Stalled);
#endif
}

CY_TEST_CASE("harness: a case that burns its own CPU is not blocked") {
    if (!cy::test::budget_measures_host_blocking()) {
        CY_TEST_MESSAGE(
            "this platform does not report host blocking; the stall ceiling is unchanged");
        return;
    }
#if defined(__linux__)
    // Running and runnable are not blocked. A case that spins is charged its CPU by the budget and
    // its preemption by the runqueue clock; the fourth clock must add nothing to the picture, or a
    // case that is merely slow would be explained as a case that was waiting for the disk.
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
#endif
}

// --- THE GATES' PROBES, THROUGH THE REAL GUARD ---------------------------------------------------
//
// Every case above re-enacts the guard's arithmetic on a window this file measured. These run
// stall_probe.cpp — real `CY_TEST_CASE`s under the unit tier's budget — as a child process and read
// the verdict the guard itself printed. CY_TEST_BUDGET_SCALE=0.25 makes the budget 0.25 ms and the
// ceiling exactly 25 ms, so each 300 ms case is twelve times over it. `757b3d9` excused the vfork
// case with no pressure at all; `4a1ad21` excused it beside the case's own readers; `23b0370`
// excused it beside the case's own ORPHANED readers. Each is `stalled:` and none may ever be
// called `contended:`.
//
// SINCE M11.c'S NINTH CLOSE (the owner's option B) a stall FAILS the case only inside a verified
// `cy_quiet_host`, so each probe is judged twice. With the marker stripped from its environment it
// must PASS and print its `stalled:` diagnosis marked "not enforced: not on a quiet host" — and
// that half runs on every host. With this process's own marker inherited it must FAIL as
// `stalled:`, "enforced: inside cy_quiet_host" — and that half runs only when this suite is itself
// inside the wrapper, as `m0:test` runs it; `smoke.quiet_host_marker` starts the wrapper itself.
// A spin over its CPU budget fails in both.

namespace {

#if defined(__linux__)
constexpr const char* kNotEnforced = "not enforced: not on a quiet host";
constexpr const char* kEnforced = "enforced: inside cy_quiet_host";

/// What the probe's environment says about the quiet host.
enum class Marker {
    /// No `CY_QUIET_HOST`: the probe was not started by the wrapper.
    Stripped,
    /// This process's own, verified or not: the probe is a descendant of whatever this is.
    Inherited,
    /// The text given, and nothing else.
    Forged,
};

/// What a probe run printed, and how it ended.
struct ProbeRun {
    bool started = false;
    int status = 0;
    std::string output;
};

struct ProbeSetting {
    const char* scale = "0.25";
    const char* extra = nullptr;
    Marker marker = Marker::Stripped;
    const char* forged = nullptr;
};

[[nodiscard]] bool starts_with(const char* entry, const char* prefix) {
    return std::strncmp(entry, prefix, std::strlen(prefix)) == 0;
}

ProbeRun run_probe(const char* test_case, const ProbeSetting& setting) {
    ProbeRun run;
    int pipe_ends[2];
    if (::pipe(pipe_ends) != 0) {
        return run;
    }
    // The child's environment is built here rather than by `setenv`, which would change the scale
    // this process's own guard re-reads.
    const std::string marker_prefix = std::string(cy::test::kQuietHostMarkerVariable) + "=";
    std::vector<std::string> variables;
    for (char** entry = environ; *entry != nullptr; ++entry) {
        if (starts_with(*entry, "CY_TEST_BUDGET_SCALE=")) {
            continue;
        }
        if (setting.marker != Marker::Inherited && starts_with(*entry, marker_prefix.c_str())) {
            continue;
        }
        variables.emplace_back(*entry);
    }
    variables.emplace_back(std::string("CY_TEST_BUDGET_SCALE=") + setting.scale);
    if (setting.marker == Marker::Forged) {
        variables.emplace_back(marker_prefix + setting.forged);
    }
    if (setting.extra != nullptr) {
        variables.emplace_back(setting.extra);
    }
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

[[nodiscard]] bool has(const ProbeRun& run, const char* text) {
    return run.output.find(text) != std::string::npos;
}

/// Outside a verified wrapper: the case PASSED, and its stall was reported, never as contended.
void expect_stall_reported(const char* test_case, const ProbeSetting& setting) {
    const ProbeRun run = run_probe(test_case, setting);
    CY_TEST_MESSAGE(test_case << " (no trusted marker):\n" << run.output);
    CY_REQUIRE(run.started);
    CY_CHECK(WIFEXITED(run.status));
    CY_CHECK_EQ(WEXITSTATUS(run.status), 0);
    CY_CHECK(has(run, "stalled:"));
    CY_CHECK(has(run, kNotEnforced));
    CY_CHECK_FALSE(has(run, kEnforced));
    CY_CHECK_FALSE(has(run, "contended:"));
}

/// The case FAILED, said why with `verdict`, named the ceiling's state, and never said contended.
void expect_probe_failed(const char* test_case, const char* verdict, const ProbeSetting& setting,
                         const char* state) {
    const ProbeRun run = run_probe(test_case, setting);
    CY_TEST_MESSAGE(test_case << ":\n" << run.output);
    CY_REQUIRE(run.started);
    CY_CHECK(WIFEXITED(run.status));
    CY_CHECK_NE(WEXITSTATUS(run.status), 0);
    CY_CHECK(has(run, verdict));
    CY_CHECK(has(run, state));
    CY_CHECK_FALSE(has(run, "contended:"));
}

/// Both halves of a stall probe: reported and passed with no marker, failed with a trusted one.
void expect_probe_stalled(const char* test_case, ProbeSetting setting = {}) {
    setting.marker = Marker::Stripped;
    expect_stall_reported(test_case, setting);
    if (!cy::test::stall_ceiling_enforced()) {
        CY_TEST_MESSAGE("this suite is not inside a verified cy_quiet_host ("
                        << cy::test::quiet_host().reason
                        << "), so the enforced half is smoke.quiet_host_marker's");
        return;
    }
    setting.marker = Marker::Inherited;
    expect_probe_failed(test_case, "stalled:", setting, kEnforced);
}
#endif

}  // namespace

CY_TEST_CASE("harness: the gate's probe — a case held by its own vfork child is a stall") {
#if defined(__linux__)
    // M11.c's fifth close: `757b3d9` reported this case as "contended: ... 300.040 ms blocked on
    // the host's disk or a page fault" and passed it. It is a stall: failed inside the wrapper,
    // reported outside it.
    expect_probe_stalled("probe: a case whose own vfork child holds it");
#else
    CY_TEST_MESSAGE("the probe's vfork case is Linux's; nothing to run here");
#endif
}

CY_TEST_CASE(
    "harness: the gate's probe — a case held by its own vfork child while its own threads read "
    "the disk is a stall") {
#if defined(__linux__)
    // M11.c's sixth close: `4a1ad21` reported this case as "contended: ... 212.557 ms ... was the
    // host's I/O pressure" and passed it. It is a stall, whatever the rest of the host does.
    //
    // THE CEILING IS 100 ms HERE, a third of the window, which is the gate's own arithmetic: at
    // scale 1 the unit budget is 1 ms, and the case's thread spends about half of that creating
    // the foreman and waiting on it.
    expect_probe_stalled(
        "probe: a case whose own vfork child holds it while its own threads read the disk",
        {.scale = "1"});
#else
    CY_TEST_MESSAGE("the probe's vfork case is Linux's; nothing to run here");
#endif
}

CY_TEST_CASE(
    "harness: the gate's probe — a case held by its own vfork child while its own ORPHANED "
    "readers press the disk is a stall") {
#if defined(__linux__)
    // M11.c's seventh close, and the case that ended the allowance. The same readers as above,
    // but PROCESSES double-forked and re-parented to init, so that no census of the case's own
    // tree could see them: `23b0370` excused 208 ms of this case's wait through the real guard
    // and passed it as `contended`. It is a stall: nothing inside the process can tell its own
    // orphans from another terminal's build, so the guard excuses neither.
    // The readers are started before the probe's `main`, as the gate's were, and only when asked
    // for: the variable is what asks.
    //
    // THE CEILING IS 200 ms HERE, two thirds of the window. `23b0370` excused this case 194 to
    // 235 ms on this host, alone — the gate saw 208 — which straddles the 200 ms that a 100 ms
    // ceiling would need, so at scale 1 the old guard passed it four runs in five and the proof
    // would be a coin. At scale 2 the question is whether the old guard excused more than 100 ms
    // of its own orphans' pressure, which it always did; the guard now excuses nothing, so the
    // case is a stall at any ceiling under its 300 ms window.
    expect_probe_stalled(
        "probe: a case whose own vfork child holds it while its own orphaned readers press the "
        "disk",
        {.scale = "2", .extra = "CY_STALL_PROBE_ORPHANS=1"});
#else
    CY_TEST_MESSAGE("the probe's vfork case is Linux's; nothing to run here");
#endif
}

CY_TEST_CASE("harness: the gate's probe — a held mutex is a stall, a spin over budget everywhere") {
#if defined(__linux__)
    expect_probe_stalled("probe: a case waiting on a mutex another thread holds");
    // THE CPU BUDGET IS ENFORCED ON EVERY HOST: outside the wrapper as well as inside it.
    expect_probe_failed("probe: a case that spins", "over budget:", {}, kNotEnforced);
    if (cy::test::stall_ceiling_enforced()) {
        expect_probe_failed("probe: a case that spins",
                            "over budget:", {.marker = Marker::Inherited}, kEnforced);
    }
#else
    CY_TEST_MESSAGE("the probe is run as a POSIX child process; nothing to run here");
#endif
}

CY_TEST_CASE("harness: a forged quiet-host marker is not trusted, and the stall is only reported") {
#if defined(__linux__)
    // Option B's forgeries. The harness trusts `CY_QUIET_HOST=<pid>:<start ticks>` only when the
    // pid is a live ANCESTOR, started at that tick, whose executable is `cy_quiet_host`; a marker
    // exported by hand satisfies none of that. Each forgery below breaks exactly one rule, and the
    // probe's vfork case must pass with its stall reported and "not enforced".
    const long self = static_cast<long>(::getpid());
    const unsigned long long ticks = cy::test::process_start_ticks(self);
    CY_REQUIRE_NE(ticks, 0ULL);

    // A live process that is not an ancestor of the probe: a sleep this case starts and kills by
    // its pid.
    pid_t sleeper = 0;
    char sleep_program[] = "sleep";
    char sleep_seconds[] = "30";
    char* sleep_argv[] = {sleep_program, sleep_seconds, nullptr};
    CY_REQUIRE_EQ(::posix_spawnp(&sleeper, "sleep", nullptr, nullptr, sleep_argv, environ), 0);
    const unsigned long long sleeper_ticks = cy::test::process_start_ticks(sleeper);

    const std::string malformed = "yes";
    const std::string dead = "2147483647:1";
    const std::string not_the_wrapper = std::to_string(self) + ":" + std::to_string(ticks);
    const std::string reused = std::to_string(self) + ":" + std::to_string(ticks + 1);
    const std::string unrelated =
        std::to_string(static_cast<long>(sleeper)) + ":" + std::to_string(sleeper_ticks);
    for (const std::string* forged : {&malformed, &dead, &not_the_wrapper, &reused, &unrelated}) {
        CY_TEST_MESSAGE("forged marker: " << *forged);
        expect_stall_reported("probe: a case whose own vfork child holds it",
                              {.marker = Marker::Forged, .forged = forged->c_str()});
    }
    ::kill(sleeper, SIGKILL);
    int status = 0;
    CY_CHECK_EQ(::waitpid(sleeper, &status, 0), sleeper);
#else
    CY_TEST_MESSAGE("the marker is verified through /proc; nothing to forge here");
#endif
}
