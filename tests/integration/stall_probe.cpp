// SPDX-License-Identifier: MIT
// THE GATE'S PROBE, KEPT: cases that are MEANT to trip the budget guard, run as a child process by
// test_budget_contention.cpp, which asserts on how the guard judged them.
//
// M11.c's fifth close refuted `757b3d9` with a probe like this one, linked against the tree's
// harness: a case whose own `vfork` child held it for 300 ms on an idle host was failed as
// `stalled:` before that commit and reported and passed as `contended:` after it. The probe lived
// in a scratch directory and was lost. It is here now, as a binary no CTest entry runs directly —
// every case in it fails by design — so that the harness's own suite can run it and read the
// verdict the REAL guard reached, through the real `CY_TEST_CASE`, rather than a re-enactment of
// the guard's arithmetic.
//
// It is compiled with the unit tier's budget, and the driver sets CY_TEST_BUDGET_SCALE so the
// ceiling is known exactly. Nothing here uses anything but the public test vocabulary, so the same
// file builds against the harness of any revision — which is how the regression is shown red on
// `757b3d9`, `4a1ad21` and `23b0370`, and green after each.

#include <cy/test/fixtures.h>
#include <cy/test/test.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#    include <fcntl.h>
#    include <sys/types.h>
#    include <sys/wait.h>
#    include <unistd.h>

#    include <csignal>
#    include <cstddef>
#    include <cstdint>
#    include <cstdlib>
#    include <ctime>
#endif

namespace {

constexpr auto kHeld = std::chrono::milliseconds(300);

#if defined(__linux__)
/// Sleeps in the parent of a `vfork` for `kHeld`: an UNINTERRUPTIBLE wait the case causes itself.
/// The child only calls async-signal-safe functions before `_exit`. vfork IS the subject, so
/// posix_spawn, which the check suggests, would hide what is measured.
void hold_in_vfork() {
    timespec held{0, static_cast<long>(std::chrono::nanoseconds(kHeld).count())};
    // NOLINTNEXTLINE(bugprone-unsafe-functions,clang-analyzer-security.insecureAPI.vfork)
    const pid_t child = ::vfork();
    if (child == 0) {
        // NOLINTNEXTLINE(clang-analyzer-unix.Vfork): nanosleep is async-signal-safe
        while (::nanosleep(&held, &held) != 0) {
        }
        ::_exit(0);
    }
    CY_REQUIRE(child > 0);
    int status = 0;
    CY_CHECK(::waitpid(child, &status, 0) == child);
}

constexpr std::size_t kBlock = 4096;
constexpr std::size_t kFileBytes = std::size_t{4} << 20U;

/// Writes a 4 MiB file with O_DIRECT so that nothing of it is in the page cache. False when the
/// filesystem refuses O_DIRECT.
bool write_uncached(const std::string& path) {
    auto* chunk = static_cast<unsigned char*>(std::aligned_alloc(kBlock, kFileBytes));
    if (chunk == nullptr) {
        return false;
    }
    std::fill(chunk, chunk + kFileBytes, static_cast<unsigned char>(0x5A));
    const int writer =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT | O_CLOEXEC, 0600);
    const bool written =
        writer >= 0 && ::write(writer, chunk, kFileBytes) == static_cast<::ssize_t>(kFileBytes);
    if (writer >= 0) {
        ::close(writer);
    }
    std::free(chunk);
    return written;
}

/// Reads scattered 4 KiB blocks of an uncached file with O_DIRECT until `deadline`: every read
/// goes to the device and the thread waits for it uninterruptibly. Returns how many reads the
/// device answered.
unsigned long long read_uncached_until(const std::string& path,
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

/// Sixteen threads reading an uncached file in a directory of their own, all of it made, started,
/// joined and removed by a FOREMAN thread so that the CASE'S thread spends no CPU on any of it:
/// this probe runs under a 0.25 ms budget, and a temporary directory, a 4 MiB file or sixteen
/// thread creations at -O0 each cost more than that. What the case's thread does is hold in its
/// vfork; the reading is its process's own, and its process's own is the case's.
class OwnReaders {
public:
    OwnReaders()
        : foreman_([this]() {
              const cy::test::TempDir directory{"stall-probe-helpers"};
              const std::string path = directory.valid() ? directory.file("shared.bin") : "";
              written_.set_value(!path.empty() && write_uncached(path));
              const auto deadline = go_.get_future().get();
              std::vector<std::thread> readers;
              std::vector<unsigned long long> reads(16, 0);
              readers.reserve(reads.size());
              for (std::size_t index = 0; index < reads.size(); ++index) {
                  readers.emplace_back([&reads, &path, index, deadline]() {
                      reads[index] =
                          read_uncached_until(path, deadline, static_cast<unsigned>(index));
                  });
              }
              for (std::size_t index = 0; index < readers.size(); ++index) {
                  readers[index].join();
                  total_ += reads[index];
              }
          }) {}
    ~OwnReaders() = default;
    OwnReaders(const OwnReaders&) = delete;
    OwnReaders& operator=(const OwnReaders&) = delete;

    /// True once the foreman has the file on the device; false when the filesystem refused.
    bool written() { return written_.get_future().get(); }

    /// Starts the readers, until `deadline`.
    void start(std::chrono::steady_clock::time_point deadline) { go_.set_value(deadline); }

    /// Joins the foreman and returns how many reads the device answered between the sixteen.
    unsigned long long total() {
        foreman_.join();
        return total_;
    }

private:
    std::promise<bool> written_;
    std::promise<std::chrono::steady_clock::time_point> go_;
    unsigned long long total_ = 0;
    std::thread foreman_;
};

/// Nanoseconds since the monotonic clock's epoch, from the one call a forked child of a threaded
/// process may make: `clock_gettime` is async-signal-safe, `std::chrono` is not promised to be.
std::uint64_t monotonic_ns() noexcept {
    timespec now{};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return (static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL) +
           static_cast<std::uint64_t>(now.tv_nsec);
}

/// A reader PROCESS: scattered uncached reads of `path` until the monotonic `deadline_ns`, then
/// exit. Only system calls, because this runs in the child of a `fork` from a threaded process,
/// where another thread may hold any lock the child inherited. `alarm` bounds it whatever happens
/// to its parent.
[[noreturn]] void read_uncached_process(const char* path, std::uint64_t deadline_ns,
                                        unsigned seed) {
    ::alarm(20);
    // The child inherits the parent's aligned buffer rather than allocating one: `aligned_alloc`
    // takes the allocator's lock, which a sibling thread of the parent may hold.
    alignas(kBlock) static unsigned char block[kBlock];
    const int reader = ::open(path, O_RDONLY | O_DIRECT | O_CLOEXEC);
    std::uint64_t state = 0x9E3779B97F4A7C15ULL + seed;
    while (reader >= 0 && monotonic_ns() < deadline_ns) {
        state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
        const auto offset = static_cast<::off_t>((state >> 33U) % (kFileBytes / kBlock));
        if (::pread(reader, block, kBlock, offset * static_cast<::off_t>(kBlock)) !=
            static_cast<::ssize_t>(kBlock)) {
            break;
        }
    }
    ::_exit(0);
}

/// Sixteen reader PROCESSES that are NOT the case's children: forked by a short-lived child of
/// this process, which writes their pids down a pipe and exits, so that every reader is
/// re-parented to init (or the nearest subreaper). That is the shape of M11.c's seventh close's
/// probe — helpers no census of the case's own tree can see, and no instrument inside the process
/// can tell from another terminal's build.
///
/// STARTED BEFORE `main`, as the gate's probe started them, and only when the driver asks for it
/// with `CY_STALL_PROBE_ORPHANS` in the environment: the readers must already be pressing the
/// device when the case's first sample is taken, and the case's own wall clock must be nothing but
/// its vfork — a file written and a 200 ms warm-up inside the case would be 250 ms of the case's
/// own waiting, and the verdict would say "stalled" for the wrong reason. Every other probe case
/// runs in a process of its own without the variable, so none of them shares the disk with these.
/// The readers run past the case's window and end on their own deadline (`alarm` bounds them
/// whatever happens); they are init's to reap, not this process's, and a pid that may already
/// have been reused is not one to send a signal to.
class OrphanedReaders {
public:
    OrphanedReaders() : directory_("stall-probe-orphans") {
        const std::string path = directory_.valid() ? directory_.file("shared.bin") : "";
        if (path.empty() || !write_uncached(path)) {
            return;
        }
        const std::uint64_t deadline_ns =
            monotonic_ns() +
            static_cast<std::uint64_t>(std::chrono::nanoseconds(kReaderRun).count());
        spawn(path, deadline_ns);
        // Let the device see the readers before the first case starts.
        std::this_thread::sleep_for(kReaderWarmup);
    }
    ~OrphanedReaders() = default;
    OrphanedReaders(const OrphanedReaders&) = delete;
    OrphanedReaders& operator=(const OrphanedReaders&) = delete;

    /// How many readers were started: sixteen, or fewer when the filesystem refused O_DIRECT or
    /// a fork failed.
    [[nodiscard]] std::size_t count() const noexcept { return orphans_.size(); }

private:
    static constexpr unsigned kReaders = 16;
    /// Longer than the case's window by a margin: the readers must outlive the vfork.
    static constexpr auto kReaderRun = std::chrono::milliseconds(1500);
    static constexpr auto kReaderWarmup = std::chrono::milliseconds(200);

    void spawn(const std::string& path, std::uint64_t deadline_ns) {
        int pipe_ends[2];
        if (::pipe(pipe_ends) != 0) {
            return;
        }
        const pid_t intermediary = ::fork();
        if (intermediary == 0) {
            ::close(pipe_ends[0]);
            for (unsigned index = 0; index < kReaders; ++index) {
                const pid_t reader = ::fork();
                if (reader == 0) {
                    ::close(pipe_ends[1]);
                    read_uncached_process(path.c_str(), deadline_ns, index);
                }
                // A reader whose pid never reaches the parent is not counted; a short write ends
                // it here rather than leaving an uncounted reader on the device.
                if (reader > 0 && ::write(pipe_ends[1], &reader, sizeof(reader)) !=
                                      static_cast<::ssize_t>(sizeof(reader))) {
                    (void)::kill(reader, SIGKILL);
                }
            }
            ::_exit(0);
        }
        ::close(pipe_ends[1]);
        if (intermediary > 0) {
            pid_t reader = 0;
            while (::read(pipe_ends[0], &reader, sizeof(reader)) == sizeof(reader)) {
                orphans_.push_back(reader);
            }
            int status = 0;
            (void)::waitpid(intermediary, &status, 0);
        }
        ::close(pipe_ends[0]);
    }

    cy::test::TempDir directory_;
    std::vector<pid_t> orphans_;
};

OrphanedReaders& orphaned_readers() {
    static OrphanedReaders instance;
    return instance;
}

/// Before `main`, when the driver asked for the readers; false in every other probe process.
const bool orphans_started =
    std::getenv("CY_STALL_PROBE_ORPHANS") != nullptr && (orphaned_readers(), true);
#endif

}  // namespace

CY_TEST_CASE("probe: a case whose own vfork child holds it") {
#if defined(__linux__)
    // The parent of a `vfork` sleeps UNINTERRUPTIBLY until the child execs or exits, so the fourth
    // clock reads it as `D` for the whole 300 ms. Nothing else on the host is involved: this is the
    // case waiting on itself.
    hold_in_vfork();
#else
    CY_TEST_MESSAGE("no vfork on this platform");
#endif
}

CY_TEST_CASE("probe: a case whose own vfork child holds it while its own threads read the disk") {
#if defined(__linux__)
    // M11.c'S SIXTH CLOSE. The same wait, while sixteen of the case's OWN threads keep the device
    // busy with uncached reads: the host's I/O pressure is high for the whole window, and every
    // bit of it is this process's. `4a1ad21` subtracted only the calling thread's share of that
    // pressure, called the rest the host's, and passed this case as `contended` with 212.557 ms
    // excused on a quiet host. It is a stall: the guard excuses no uninterruptible wait at all.
    OwnReaders readers;
    // A filesystem that refuses O_DIRECT cannot make the helpers press the disk, and says so.
    // The foreman is joined by `total()` whatever `written()` said, so a REQUIRE here would
    // leave a thread unjoined; the case fails and returns instead.
    if (!readers.written()) {
        readers.start(std::chrono::steady_clock::now());
        (void)readers.total();
        CY_TEST_FAIL(
            "no temporary directory, or this filesystem refuses O_DIRECT; the helpers "
            "cannot press the disk here");
        return;
    }
    readers.start(std::chrono::steady_clock::now() + kHeld);
    hold_in_vfork();
    // The premise, so that helpers that never reached the device are reported rather than passed
    // as a probe whose helpers did nothing.
    CY_CHECK_GT(readers.total(), 0ULL);
#else
    CY_TEST_MESSAGE("no vfork on this platform");
#endif
}

CY_TEST_CASE(
    "probe: a case whose own vfork child holds it while its own orphaned readers press the disk") {
#if defined(__linux__)
    // M11.c'S SEVENTH CLOSE. The same wait, while sixteen reader PROCESSES the case started —
    // double-forked, so that init and not this process is their parent — keep the device busy
    // with uncached reads. `23b0370` took a census of the case's own process tree and subtracted
    // its share of the host's I/O pressure; these readers were outside the tree it could walk,
    // their pressure was "other processes'", and the guard excused 208 ms and passed the case as
    // `contended`. There is no instrument inside a process that tells its own orphans from a
    // build in another terminal, which is why the guard now excuses neither: this is a stall, and
    // the quiet host the criteria assume is checked from outside.
    // The readers were started before `main` and are pressing the device now; a driver that
    // forgot to ask for them, or a filesystem that refused O_DIRECT, is reported rather than
    // passed as a probe whose readers did nothing.
    CY_REQUIRE(orphans_started);
    CY_REQUIRE_EQ(orphaned_readers().count(), std::size_t{16});
    hold_in_vfork();
#else
    CY_TEST_MESSAGE("no vfork on this platform");
#endif
}

CY_TEST_CASE("probe: a case waiting on a mutex another thread holds") {
    // A futex wait is an interruptible sleep: the fourth clock reads `S`, so nothing is excused.
    std::mutex held;
    std::condition_variable locked;
    bool is_locked = false;
    std::mutex handshake;
    std::thread holder([&]() {
        const std::lock_guard<std::mutex> hold(held);
        {
            const std::lock_guard<std::mutex> tell(handshake);
            is_locked = true;
        }
        locked.notify_one();
        std::this_thread::sleep_for(kHeld);
    });
    {
        std::unique_lock<std::mutex> wait(handshake);
        locked.wait(wait, [&]() { return is_locked; });
    }
    {
        const std::lock_guard<std::mutex> take(held);
    }
    holder.join();
}

CY_TEST_CASE("probe: a case that spins") {
    // Running is not waiting: the CPU budget catches this, whatever the host is doing.
    const auto until = std::chrono::steady_clock::now() + kHeld;
    volatile unsigned long long sink = 0;
    while (std::chrono::steady_clock::now() < until) {
        sink = sink + 1U;
    }
}
