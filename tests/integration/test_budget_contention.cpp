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
#    include <sys/mman.h>
#    include <sys/resource.h>
#    include <unistd.h>

#    include <cstdint>
#    include <cstdlib>
#    include <ctime>
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
// M11.c's FOURTH CLOSE: the host's DISK, which the runqueue clock cannot see
// ================================================================================================
//
// `unit.determinism`'s first case held the suite for 655.4 ms with 0.21 ms of CPU and zero runqueue
// wait, beside a heavy build, and passed three runs in three alone: a thread waiting for the disk
// is not runnable, so the third clock never moved. The fourth clock, `blocked_on_host_ns()`,
// samples the case's own scheduler state and counts the time it spends in an uninterruptible wait.
// These three cases are its empirical half, in both directions, measured through the verdict the
// guard itself uses:
//
//  1. a case that waits for the disk accumulates host blocking, and a ceiling it would otherwise
//     blow is excused;
//  2. a case that SLEEPS while another thread keeps the same disk busy accumulates none, and is
//     still a stall — the machine being busy with I/O excuses nothing the case did not wait for;
//  3. a case that burns its own CPU accumulates none either.
//
// THE DISK WAIT IS MADE WITH O_DIRECT, which is the one way a test can wait for the device without
// needing root to drop the page cache: every read goes to the device and the thread waits for it in
// `io_schedule()`, uninterruptibly, exactly as a major page fault does. The window runs for a fixed
// wall-clock time rather than a fixed number of reads, so a fast device and a slow one produce the
// same length of evidence. A filesystem that refuses O_DIRECT (tmpfs) reports that rather than
// asserting on a condition it could not create.
//
// THE CEILING IS THREE QUARTERS OF THE WINDOW, so the wall clock is always over it and the verdict
// turns on the host's share alone: excused when the host accounts for more than a quarter of the
// window, stalled otherwise. That is the same low bar the runqueue case above uses, for the same
// reason — the claim is that the clock MOVES, not that a device divides time a particular way.

namespace {

#if defined(__linux__)
constexpr std::size_t kBlock = 4096;
constexpr std::size_t kFileBytes = std::size_t{4} << 20U;
constexpr std::size_t kWriteChunk = std::size_t{1} << 20U;
constexpr auto kWindow = std::chrono::milliseconds(150);

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

void report(const char* what, const Window& window) {
    CY_TEST_MESSAGE(std::string(what)
                    << ": " << (static_cast<double>(window.wall_ns) / 1e6) << " ms wall, "
                    << (static_cast<double>(window.cpu_ns) / 1e6) << " ms CPU, "
                    << (static_cast<double>(window.contended_ns) / 1e6) << " ms runqueue, "
                    << (static_cast<double>(window.blocked_ns) / 1e6) << " ms blocked on the host, "
                    << window.reads << " reads or touches");
}

cy::test::StallVerdict verdict_of(const Window& window) {
    return cy::test::stall_verdict(window.wall_ns, window.contended_ns + window.blocked_ns,
                                   (window.wall_ns / 4U) * 3U);
}
#endif

}  // namespace

CY_TEST_CASE(
    "harness: a case waiting for the host's disk accumulates host blocking, and is excused") {
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
    report("disk wait", window);
    CY_REQUIRE(window.reads > 0U);
    CY_REQUIRE(window.wall_ns >= 100'000'000ULL);

    // THE ASSERTIONS. The clock moved by a real part of the window — the measured figure on this
    // project's host is most of it — and the verdict the guard would reach is "the host", not "the
    // case". Without the fourth clock the host's share is the runqueue wait alone, which on an idle
    // machine is nothing, and this window is a stall.
    CY_CHECK_GT(window.blocked_ns, window.wall_ns / 4U);
    CY_CHECK(verdict_of(window) == cy::test::StallVerdict::Contended);
#endif
}

CY_TEST_CASE("harness: a case waiting on major page faults accumulates host blocking") {
    if (!cy::test::budget_measures_host_blocking()) {
        CY_TEST_MESSAGE(
            "this platform does not report host blocking; the stall ceiling is unchanged");
        return;
    }
#if defined(__linux__)
    // THE FLAKE'S OWN MECHANISM. A case that does nothing but run its own code for the first time
    // takes a major fault for every page a build has pushed out of the page cache, and waits for
    // the device on each one. This makes that happen on purpose, through a mapping, and asserts
    // that the fourth clock sees it the way it sees an explicit read.
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
    report("major page faults", window);
    CY_TEST_MESSAGE(faults << " major faults");
    CY_REQUIRE(window.wall_ns >= 100'000'000ULL);
    // The premise: the pages really were not in memory. A kernel that ignored the eviction would
    // serve every touch from the cache, and there would be nothing for the clock to see.
    CY_REQUIRE(faults > 0U);

    CY_CHECK_GT(window.blocked_ns, window.wall_ns / 4U);
    CY_CHECK(verdict_of(window) == cy::test::StallVerdict::Contended);
#endif
}

CY_TEST_CASE("harness: a case that sleeps while the disk is busy is still a stall") {
    if (!cy::test::budget_measures_host_blocking()) {
        CY_TEST_MESSAGE(
            "this platform does not report host blocking; the stall ceiling is unchanged");
        return;
    }
#if defined(__linux__)
    // THE OTHER DIRECTION, AND THE ONE THAT KEEPS THE CEILING HONEST. Another thread of this very
    // process keeps the same device busy with uncached reads for the whole window, so the host IS
    // loaded with I/O — a system-wide pressure figure would move by most of the window here — while
    // the case itself only sleeps. The case did not wait for the disk, so nothing is excused.
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
    CY_REQUIRE(window.wall_ns >= 100'000'000ULL);

    CY_CHECK_LT(window.blocked_ns, window.wall_ns / 10U);
    CY_CHECK(verdict_of(window) == cy::test::StallVerdict::Stalled);
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
    CY_REQUIRE(window.wall_ns >= 100'000'000ULL);

    CY_CHECK_LT(window.blocked_ns, window.wall_ns / 10U);
    // With the runqueue left out, the host's blocking alone excuses nothing here.
    CY_CHECK(
        cy::test::stall_verdict(window.wall_ns, window.blocked_ns, (window.wall_ns / 4U) * 3U) ==
        cy::test::StallVerdict::Stalled);
#endif
}
