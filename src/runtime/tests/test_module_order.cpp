// Module registration order, across a hundred processes. Task 4.5.
//
// `engine-architecture` fixes eleven startup stages, four of which are "modules at level <L>".
// tests/smoke/test_startup_order.cpp already proves the eleven are identical across a hundred
// processes; this proves the same of what happens *inside* the four, which is the half M1 filled.
//
// Separate processes rather than a loop, for the reason that test gives: a fresh address space each
// time is what would expose an order that depended on an allocation address, a hash seed or the
// order of static initialisers. A hundred iterations of one loop would reproduce such an order
// faithfully and report it as deterministic.
//
// The probe registers the project graph's own modules plus five synthetic ones, added in an order
// that is neither their level order nor alphabetical. The assertion here is on the synthetic five,
// by name and in full: which modules a build enables is a build's business, but if the registry
// leaked insertion order into the result these five would come out in the order the probe added
// them. The rest of the graph is checked structurally — same output every run, and the stop journal
// the exact reverse of the start journal.

#include <cy/test/test.h>

#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#    define CY_MODULE_ORDER_POPEN _popen
#    define CY_MODULE_ORDER_PCLOSE _pclose
#else
#    include <sys/wait.h>
#    define CY_MODULE_ORDER_POPEN popen
#    define CY_MODULE_ORDER_PCLOSE pclose
#endif

namespace {

constexpr int kRuns = 100;

struct ProcessResult {
    bool ran = false;
    int exit_code = -1;
    std::string output;
};

int decode_exit_status(int raw) {
#if defined(_WIN32)
    return raw;
#else
    if (WIFEXITED(raw)) {
        return WEXITSTATUS(raw);
    }
    if (WIFSIGNALED(raw)) {
        return 128 + WTERMSIG(raw);
    }
    return -1;
#endif
}

ProcessResult run(const std::string& command) {
    ProcessResult result;
    // The command is the build-generated path to the probe plus this test's own argument. NOLINT
    // because clang-tidy is right in general and wrong here, and silencing it in the .clang-tidy
    // would silence it for the whole engine.
    // NOLINTNEXTLINE(bugprone-command-processor)
    std::FILE* pipe = CY_MODULE_ORDER_POPEN(command.c_str(), "r");
    if (pipe == nullptr) {
        return result;
    }
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result.output += buffer;
    }
    result.exit_code = decode_exit_status(CY_MODULE_ORDER_PCLOSE(pipe));
    result.ran = true;
    return result;
}

/// The words of the line beginning with `prefix`, after the prefix itself. Empty when there is no
/// such line, which fails the case that asked for it rather than silently comparing nothing.
std::vector<std::string> words_after(const std::string& output, const std::string& prefix) {
    std::vector<std::string> words;
    std::size_t start = 0;
    while (start < output.size()) {
        const std::size_t end = output.find('\n', start);
        const std::string line = output.substr(start, end - start);
        start = end == std::string::npos ? output.size() : end + 1;
        if (!line.starts_with(prefix)) {
            continue;
        }
        std::size_t cursor = prefix.size();
        while (cursor < line.size()) {
            while (cursor < line.size() && line[cursor] == ' ') {
                ++cursor;
            }
            const std::size_t word_end = line.find(' ', cursor);
            if (cursor < line.size()) {
                words.push_back(line.substr(cursor, word_end - cursor));
            }
            cursor = word_end == std::string::npos ? line.size() : word_end;
        }
        return words;
    }
    return words;
}

std::vector<std::string> only_synthetic(const std::vector<std::string>& names) {
    std::vector<std::string> found;
    for (const std::string& name : names) {
        if (name.starts_with("probe-")) {
            found.push_back(name);
        }
    }
    return found;
}

// Level, then name. Written out rather than computed, so that a change to the ordering rule shows
// up here as a diff of two explicit sequences.
const std::vector<std::string> kExpectedSynthetic = {
    "probe-alpha-core",      // Core,    'a' < 'z'
    "probe-zulu-core",       // Core
    "probe-mike-servers",    // Servers
    "probe-november-scene",  // Scene
    "probe-sierra-editor",   // Editor
};

std::string quoted(const std::string& text) {
    return "\"" + text + "\"";
}

}  // namespace

CY_TEST_CASE("module registration order is identical across 100 runs") {
    const std::string command = quoted(CY_STARTUP_PROBE) + " --modules";

    const ProcessResult first = run(command);
    CY_REQUIRE(first.ran);
    CY_REQUIRE_EQ(first.exit_code, 0);

    const std::vector<std::string> started = words_after(first.output, "modules-started ");
    const std::vector<std::string> stopped = words_after(first.output, "modules-stopped ");
    CY_REQUIRE_FALSE(started.empty());
    CY_REQUIRE_EQ(started.size(), stopped.size());

    // The probe's own two answers, so a reader of the log sees them without reconstructing them.
    CY_CHECK_EQ(words_after(first.output, "modules-balanced "), std::vector<std::string>{"yes"});
    CY_CHECK_EQ(words_after(first.output, "modules-reversed "), std::vector<std::string>{"yes"});

    const std::vector<std::string> reversed(started.rbegin(), started.rend());
    CY_CHECK(stopped == reversed);
    CY_CHECK(only_synthetic(started) == kExpectedSynthetic);

    int identical = 1;
    for (int run_index = 1; run_index < kRuns; ++run_index) {
        const ProcessResult next = run(command);
        CY_REQUIRE(next.ran);
        CY_REQUIRE_EQ(next.exit_code, 0);
        if (next.output == first.output) {
            ++identical;
        } else {
            CY_TEST_FAIL_CHECK("run " << run_index << " differs.\n  first: " << first.output
                                      << "  this:  " << next.output);
        }
    }
    CY_CHECK_EQ(identical, kRuns);
    CY_TEST_MESSAGE(identical << " of " << kRuns << " runs registered " << started.size()
                              << " module(s) in the same order");
}

CY_TEST_CASE("the eleven stages are unchanged by the four module stages having work to do") {
    // The module stages went from empty to occupied at M1. The sequence they sit in did not, and
    // this is where that is checked against the probe's *other* mode — the same binary, run without
    // --modules, must still print exactly what tests/smoke/test_startup_order.cpp compares.
    const ProcessResult bare = run(quoted(CY_STARTUP_PROBE));
    const ProcessResult with_modules = run(quoted(CY_STARTUP_PROBE) + " --modules");
    CY_REQUIRE(bare.ran);
    CY_REQUIRE(with_modules.ran);
    CY_REQUIRE_EQ(bare.exit_code, 0);
    CY_REQUIRE_EQ(with_modules.exit_code, 0);

    CY_CHECK(words_after(bare.output, "startup ") == words_after(with_modules.output, "startup "));
    CY_CHECK(words_after(bare.output, "shutdown") == words_after(with_modules.output, "shutdown"));
    CY_CHECK(words_after(bare.output, "modules-started ").empty());
}
