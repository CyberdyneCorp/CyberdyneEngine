// The framework seam, checked against the tree as it is now rather than as it was at the last
// configure.
//
// design.md §5 chose doctest on an argument about compile time, and made the choice reversible by
// putting one wrapper in front of it. A test that reaches past cy/test/test.h to the framework's
// own macros spends that reversibility silently: nothing fails, and the cost only appears on the
// day the framework is replaced. tests/CMakeLists.txt rejects such a file when a suite is declared;
// this test is the same rule applied to every file in tests/, including one added to an existing
// suite after CMake last ran.
//
// tests/harness/ is the one directory permitted to name the framework — it is the wrapper.

#include <cy/test/fixtures.h>
#include <cy/test/test.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

// Split so that neither this file nor the configure-time check in tests/CMakeLists.txt reads the
// pattern below as a violation of itself.
const char* const kIncludeToken = "#include";
const char* const kFrameworkToken =
    "doct"
    "est";

bool is_source(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    return extension == ".cpp" || extension == ".h" || extension == ".hpp" || extension == ".inl";
}

bool is_wrapper(const std::filesystem::path& path, const std::filesystem::path& root) {
    const std::filesystem::path relative = path.lexically_relative(root);
    return !relative.empty() && relative.begin()->string() == "harness";
}

struct Finding {
    std::string file;
    int line;
};

std::vector<Finding> scan(const std::filesystem::path& root, int& files_scanned) {
    std::vector<Finding> findings;
    std::error_code error;

    for (std::filesystem::recursive_directory_iterator it{root, error}, end; it != end && !error;
         it.increment(error)) {
        const std::filesystem::path& path = it->path();
        if (!it->is_regular_file(error) || !is_source(path) || is_wrapper(path, root)) {
            continue;
        }

        std::string contents;
        if (!cy::test::read_file(path.string(), contents)) {
            continue;
        }
        ++files_scanned;

        int line = 1;
        std::size_t start = 0;
        while (start <= contents.size()) {
            const std::size_t end_of_line = contents.find('\n', start);
            const std::string text = contents.substr(
                start, end_of_line == std::string::npos ? std::string::npos : end_of_line - start);
            if (text.find(kIncludeToken) != std::string::npos &&
                text.find(kFrameworkToken) != std::string::npos) {
                findings.push_back(Finding{.file = path.string(), .line = line});
            }
            if (end_of_line == std::string::npos) {
                break;
            }
            start = end_of_line + 1;
            ++line;
        }
    }

    return findings;
}

}  // namespace

CY_TEST_CASE("seam: no test outside the harness names the framework") {
    const std::filesystem::path root{CY_TESTS_SOURCE_DIR};
    CY_REQUIRE(std::filesystem::is_directory(root));

    int files_scanned = 0;
    const std::vector<Finding> findings = scan(root, files_scanned);

    // A scan that found nothing because it looked nowhere would pass forever. The count is the
    // guard on the guard.
    CY_CHECK_GE(files_scanned, 4);

    for (const Finding& finding : findings) {
        CY_TEST_FAIL_CHECK("reaches past cy/test/test.h to the framework itself: "
                           << finding.file << ":" << finding.line
                           << " — the wrapper is what makes the framework replaceable "
                              "(design.md §5).");
    }
    CY_CHECK_EQ(findings.size(), 0U);
}
