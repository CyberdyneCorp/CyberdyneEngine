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
// `757b3d9` and green after it.

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
/// vfork; the reading is its process's, which is what the census is for.
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
    // excused on a quiet host. The census of the case's tree is what makes it a stall.
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
