// cy/test/fixtures.h — the fixtures a test may depend on instead of the world outside it.
//
// `testing-and-quality` requires that time, random number generation and input are injectable
// rather than globally sourced, and that parallel tests share nothing. What that needs at M0 is a
// clock a test advances itself, a generator whose sequence is a function of its seed, and a
// directory that belongs to one test and is removed with it.
//
// The rest of the harness the specification names — a mock Platform and DisplayServer, an in-memory
// filesystem mount, network condition simulation, image comparison, state hashing — is deliberately
// absent: each is a mock of an interface that does not exist yet. tests/harness/README.md records
// which milestone brings each one.

#ifndef CY_TEST_FIXTURES_H
#define CY_TEST_FIXTURES_H

#include <cstdint>
#include <string>

namespace cy::test {

/// A clock a test drives. Nanoseconds, monotonic, and it moves only when the test moves it, so a
/// test that advances simulation supplies the step rather than depending on wall-clock time.
class DeterministicClock {
public:
    explicit DeterministicClock(std::uint64_t start_ns = 0) noexcept : now_ns_(start_ns) {}

    [[nodiscard]] std::uint64_t now_ns() const noexcept { return now_ns_; }

    void advance_ns(std::uint64_t delta_ns) noexcept { now_ns_ += delta_ns; }

    /// Advance by a frame at a fixed rate — the common case, spelled so that a test reads as the
    /// simulation it is driving.
    void advance_frames(std::uint32_t frames, std::uint32_t hz = 60) noexcept {
        now_ns_ += (1000000000ULL / hz) * frames;
    }

private:
    std::uint64_t now_ns_;
};

/// A seeded generator whose sequence depends on nothing but its seed.
///
/// xorshift64*, written out rather than taken from <random>: the standard specifies the engines'
/// sequences but not the distributions', so the same std::uniform_int_distribution can produce
/// different values on two standard libraries. A determinism fixture that is only deterministic per
/// platform is worse than none, because it passes everywhere it is run and fails where it is not.
class SeededRandom {
public:
    explicit SeededRandom(std::uint64_t seed) noexcept
        : state_(seed != 0 ? seed : 0x9e3779b97f4a7c15ULL) {}

    std::uint64_t next_u64() noexcept {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545f4914f6cdd1dULL;
    }

    /// A value in [0, bound). Rejection-free and very slightly biased for large bounds; test data
    /// does not need more, and the bias is documented rather than hidden.
    std::uint32_t next_below(std::uint32_t bound) noexcept {
        return static_cast<std::uint32_t>((next_u64() >> 32) % bound);
    }

    /// A value in [0, 1).
    double next_unit() noexcept {
        return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
    }

private:
    std::uint64_t state_;
};

/// A directory owned by one test, removed when the fixture goes out of scope.
///
/// Isolation is the point: `testing-and-quality` requires that tests running in parallel cannot
/// affect one another, and a shared scratch path is the usual way that guarantee is lost. The name
/// carries the test's own label and a counter, so a leftover directory from a crashed run names the
/// test that left it.
class TempDir {
public:
    explicit TempDir(const char* label);
    ~TempDir();

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    /// The directory, or an empty string if it could not be created — creation failure is reported
    /// as a test failure by valid(), never by throwing, since the engine builds without exceptions.
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] bool valid() const noexcept { return !path_.empty(); }

    /// A path inside the directory. Does not create anything.
    [[nodiscard]] std::string file(const char* name) const;

private:
    std::string path_;
};

/// Write `contents` to `path`, returning false rather than reporting an error out of band.
bool write_file(const std::string& path, const std::string& contents);

/// Read `path` into `out`, returning false if it could not be read.
bool read_file(const std::string& path, std::string& out);

}  // namespace cy::test

#endif  // CY_TEST_FIXTURES_H
