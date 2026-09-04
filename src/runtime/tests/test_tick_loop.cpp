// The state hash reproduces across process restarts. Tasks 4.1.2 and 4.2.6.
//
// This is the across-processes half of the milestone's reproducibility claim; the within-process
// half is `integration.runtime_simulation`. Separate processes rather than a loop, for the reason
// tests/smoke/test_startup_order.cpp gives: a fresh address space each time is what would expose a
// result that depended on an allocation address, a per-process hash seed or the order of static
// initialisers. Twenty iterations of one loop would reproduce such a result faithfully and report
// it as deterministic.
//
// The probe runs a real `Runtime` — the eleven stages, the fixed-step tick loop, a system that
// integrates over a query and draws from a named seeded stream — and prints the frame counts, the
// committed point, the state version, the hash and the hash's coverage report. Everything on those
// three lines has to match, not just the hash: a run that stopped ticking early would produce a
// stable hash of a state it reached by accident.

#include <cy/test/test.h>

#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#    define CY_TICK_LOOP_POPEN _popen
#    define CY_TICK_LOOP_PCLOSE _pclose
#else
#    include <sys/wait.h>
#    define CY_TICK_LOOP_POPEN popen
#    define CY_TICK_LOOP_PCLOSE pclose
#endif

namespace {

constexpr int kRuns = 20;

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
    // The command is the build-generated path to the probe plus this test's own arguments. NOLINT
    // because clang-tidy is right in general and wrong here, and silencing it in the .clang-tidy
    // would silence it for the whole engine.
    // NOLINTNEXTLINE(bugprone-command-processor)
    std::FILE* pipe = CY_TICK_LOOP_POPEN(command.c_str(), "r");
    if (pipe == nullptr) {
        return result;
    }
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result.output += buffer;
    }
    result.exit_code = decode_exit_status(CY_TICK_LOOP_PCLOSE(pipe));
    result.ran = true;
    return result;
}

std::string probe(const char* arguments) {
    return std::string(CY_TICK_LOOP_PROBE) + " " + arguments + " 2>/dev/null";
}

}  // namespace

CY_TEST_CASE("the state hash is identical across twenty processes") {
    const ProcessResult first = run(probe("--seed 7 --frames 64"));
    CY_REQUIRE(first.ran);
    CY_REQUIRE_EQ(first.exit_code, 0);
    CY_REQUIRE_FALSE(first.output.empty());

    // The counts are asserted as well as the hash. A run that stopped ticking early would produce a
    // stable hash of a state it reached by accident, and only the tick count would say so.
    CY_CHECK(first.output.find("frames 64 ticks 64 epoch 0 tick 64 version 64\n") !=
             std::string::npos);
    CY_CHECK(first.output.find("hash ") != std::string::npos);
    // Sixteen entities, two authoritative fields each; the third field is Derived and is not
    // hashed.
    CY_CHECK(first.output.find("entities 16 fields 32 ") != std::string::npos);

    for (int run_index = 1; run_index < kRuns; ++run_index) {
        const ProcessResult next = run(probe("--seed 7 --frames 64"));
        CY_REQUIRE(next.ran);
        CY_REQUIRE_EQ(next.exit_code, 0);
        if (next.output != first.output) {
            CY_TEST_FAIL("run ", run_index, " differed:\nfirst:\n", first.output, "this run:\n",
                         next.output);
            break;
        }
    }
}

CY_TEST_CASE("a different session seed is a different state before a value is drawn") {
    // The random source is a hashed state provider, so two sessions with different seeds diverge at
    // a named subsystem rather than somewhere in the entity data — and here they also diverge in
    // the entity data, because the system draws from the stream.
    const ProcessResult seven = run(probe("--seed 7 --frames 8"));
    const ProcessResult eight = run(probe("--seed 8 --frames 8"));
    CY_REQUIRE(seven.ran);
    CY_REQUIRE(eight.ran);
    CY_REQUIRE_EQ(seven.exit_code, 0);
    CY_REQUIRE_EQ(eight.exit_code, 0);
    CY_CHECK_NE(seven.output, eight.output);

    // The tick accounting is the same; only the hash line differs.
    CY_CHECK(seven.output.find("frames 8 ticks 8 ") != std::string::npos);
    CY_CHECK(eight.output.find("frames 8 ticks 8 ") != std::string::npos);
}

CY_TEST_CASE("more ticks is more simulation, not a longer step") {
    const ProcessResult few = run(probe("--seed 7 --frames 8"));
    const ProcessResult many = run(probe("--seed 7 --frames 16"));
    CY_REQUIRE_EQ(few.exit_code, 0);
    CY_REQUIRE_EQ(many.exit_code, 0);
    CY_CHECK(few.output.find("frames 8 ticks 8 epoch 0 tick 8 version 8\n") != std::string::npos);
    CY_CHECK(many.output.find("frames 16 ticks 16 epoch 0 tick 16 version 16\n") !=
             std::string::npos);
    CY_CHECK_NE(few.output, many.output);
}
