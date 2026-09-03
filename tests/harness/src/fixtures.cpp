// The filesystem half of the fixtures. The clock and the generator are header-only: they have no
// dependencies beyond arithmetic, and a test that wants one should not pay for a link.

#include <cy/test/fixtures.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace cy::test {
namespace {

// Every filesystem call here takes the std::error_code overload. The throwing overloads are
// unusable in this tree — the engine compiles with -fno-exceptions, so a throw from libstdc++ ends
// the process rather than the test — and reporting failure through valid() keeps a fixture that
// could not be created a test failure rather than a crash.

std::atomic<unsigned> g_counter{0};

std::string sanitise(const char* label) {
    std::string out;
    for (const char* c = label; *c != '\0'; ++c) {
        const bool safe = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                          (*c >= '0' && *c <= '9') || *c == '-' || *c == '_';
        out.push_back(safe ? *c : '-');
    }
    return out.empty() ? std::string{"test"} : out;
}

}  // namespace

TempDir::TempDir(const char* label) {
    std::error_code error;
    const std::filesystem::path base = std::filesystem::temp_directory_path(error);
    if (error) {
        std::fprintf(stderr, "cy::test::TempDir: no temporary directory: %s\n",
                     error.message().c_str());
        return;
    }

    // Unique across concurrent runs without a platform call for the process id: the counter
    // separates fixtures within a process, the clock separates processes.
    const unsigned ordinal = g_counter.fetch_add(1, std::memory_order_relaxed);
    const auto stamp = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path candidate =
        base / ("cy-test-" + sanitise(label) + "-" + std::to_string(stamp) + "-" +
                std::to_string(ordinal));

    if (!std::filesystem::create_directories(candidate, error) || error) {
        std::fprintf(stderr, "cy::test::TempDir: cannot create %s: %s\n",
                     candidate.string().c_str(), error.message().c_str());
        return;
    }
    path_ = candidate.string();
}

TempDir::~TempDir() {
    if (path_.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    if (error) {
        // Reported, not asserted: a directory that outlives its test is a leak worth seeing, and
        // failing a passing test in a destructor would report it against the wrong cause.
        std::fprintf(stderr, "cy::test::TempDir: cannot remove %s: %s\n", path_.c_str(),
                     error.message().c_str());
    }
}

std::string TempDir::file(const char* name) const {
    if (path_.empty()) {
        return {};
    }
    return (std::filesystem::path{path_} / name).string();
}

bool write_file(const std::string& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    stream.close();
    return stream.good();
}

bool read_file(const std::string& path, std::string& out) {
    // Sized read rather than a streambuf iterator: one allocation, and it does not trip GCC 13's
    // -Wnull-dereference inside <streambuf>, which this tree compiles with -Werror.
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return false;
    }
    const std::streamoff size = stream.tellg();
    if (size < 0) {
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    stream.seekg(0, std::ios::beg);
    if (size > 0) {
        stream.read(out.data(), size);
    }
    return stream.good();
}

}  // namespace cy::test
