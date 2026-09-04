#ifndef CY_CORE_ASSETS_TESTS_TEMP_DIR_H
#define CY_CORE_ASSETS_TESTS_TEMP_DIR_H
// A scratch directory for the suites that touch a real filesystem.
//
// Created under the test binary's own working directory rather than under the system temporary
// directory: CTest runs each suite in its build directory, so the artefacts of a failed run are
// beside the binary that produced them instead of somewhere a developer has to be told about.

#include <cy/core/assets/file.h>
#include <cy/test/test.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace cy::assets::test {

/// A directory that removes itself. Named after the case that made it, so a failure is traceable.
class TempDir {
public:
    explicit TempDir(const char* label) {
        static std::atomic<unsigned> serial{0};
        char buffer[512] = {};
        (void)std::snprintf(buffer, sizeof(buffer), "cy_assets_test_%s_%u", label,
                            serial.fetch_add(1));
        path_ = buffer;
        (void)fs::remove_directory_recursive(path_.c_str());
        CY_REQUIRE(fs::create_directories(path_.c_str()).has_value());
    }

    ~TempDir() { (void)fs::remove_directory_recursive(path_.c_str()); }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const char* c_str() const noexcept { return path_.c_str(); }

    /// A path inside the directory. Returned by value so a caller can hold it across calls.
    [[nodiscard]] std::string file(const char* name) const { return path_ + "/" + name; }

private:
    std::string path_;
};

}  // namespace cy::assets::test

#endif  // CY_CORE_ASSETS_TESTS_TEMP_DIR_H
