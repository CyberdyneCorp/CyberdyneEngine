// The M1 milestone gate, as a test. Tasks 5.3 and 5.4.
//
// It runs samples/01-headless-host — the artefact that exercises reflection, memory, jobs and
// assets in one program — and asserts the two things a milestone gate can assert about a whole
// process: that it did its work and exited cleanly, and that it did it the same way every time.
//
// TWO CASES, TWO CLAIMS.
//
//   The first reads the run's own report: the package was cooked and loaded, the schedule derived
//   two batches from three access declarations, no reflected lookup happened inside a hot region,
//   and the budget tree compares a real figure against a real target with nothing over budget. A
//   budget report that named no domain, or that reported a domain using more than it was given,
//   would pass a test that only checked the exit code.
//
//   The second runs it a hundred times, in a hundred processes, and requires the startup order, the
//   shutdown order and the simulation's checksum to be identical in all of them. Separate processes
//   rather than a loop: a fresh address space each time is what would expose an order that depended
//   on an allocation address, a hash seed or the order of static initialisers, and a hundred
//   iterations of one loop would reproduce such an order faithfully and call it deterministic.
//
// THE BUDGET ROWS ARE EXCLUDED FROM THE COMPARISON, and that is a finding rather than an
// accommodation: a domain's PEAK is the high-water mark of a sum that several threads add to at
// once, so it legitimately differs by when a worker happened to allocate. Live bytes, the checksum
// and both journals do not, and those are what is compared.
//
// NOTHING ELSE MAY JOIN THAT EXCLUSION. The `jobs` line was the second figure to differ across
// runs — 56 tasks instead of 57, about once in two hundred runs on a loaded machine — and the
// answer was to report a figure that is ordered rather than to stop comparing it: `tasks_executed`
// is incremented after the release store that unblocks the wait, so a host that waits for the last
// task and reads the counter in the next statement can miss it, while `tasks_submitted` is
// incremented before the body runs and is exact for every task the host waited on. The sample
// prints the latter, and the case below requires the line to still be inside the compared text, so
// that reverting the sample fails here rather than flaking one run in fifty.

#include <cy/test/fixtures.h>
#include <cy/test/test.h>

#include <cstdio>
#include <string>
#include <vector>

#include "process.h"

namespace {

using cy::test::smoke::ProcessResult;

/// Run the sample with its own content directory, so two cases running at once cannot share a file.
ProcessResult run_sample(const cy::test::TempDir& content, const char* extra = "") {
    return cy::test::smoke::run(cy::test::smoke::quoted(CY_SAMPLE_HEADLESS_HOST) + " --content " +
                                cy::test::smoke::quoted(content.path()) + " " + extra);
}

bool contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

/// The first line beginning with `prefix`, or an empty string.
std::string line_with(const std::string& text, const char* prefix) {
    const std::string::size_type start = text.find(prefix);
    if (start == std::string::npos) {
        return {};
    }
    const std::string::size_type end = text.find('\n', start);
    return text.substr(start, end == std::string::npos ? end : end - start);
}

/// Everything the run reported except its budget rows. See the header: a peak is a high-water mark
/// over concurrent allocations and is the one figure that is legitimately not reproducible.
std::string without_budget_rows(const std::string& text) {
    std::string kept;
    std::string::size_type start = 0;
    while (start < text.size()) {
        const std::string::size_type end = text.find('\n', start);
        const std::string::size_type length =
            end == std::string::npos ? text.size() - start : end - start + 1;
        const std::string line = text.substr(start, length);
        if (line.find(": budget ") == std::string::npos) {
            kept += line;
        }
        start += length;
    }
    return kept;
}

/// One row of the budget report, as the sample prints it.
struct BudgetRow {
    bool found = false;
    char domain[32] = {};
    char kind[16] = {};
    unsigned long long target_mib = 0;
    unsigned long long live = 0;
    unsigned long long peak = 0;
    double utilisation = 0.0;
};

BudgetRow budget_row(const std::string& output, const char* domain) {
    const std::string prefix = std::string{"01-headless-host: budget   "} + domain + " ";
    const std::string line = line_with(output, prefix.c_str());
    BudgetRow row;
    if (line.empty()) {
        return row;
    }
    // sscanf, and its result is checked: `fields == 6` is exactly the conversion error this reads
    // for. The suggested strtoull would need six calls and its own tokeniser to say the same thing.
    // NOLINTBEGIN(bugprone-unchecked-string-to-number-conversion)
    const int fields =
        std::sscanf(line.c_str() + std::string{"01-headless-host: budget   "}.size(),
                    "%31s %15s target %llu MiB live %llu B peak %llu B %lf%%", row.domain, row.kind,
                    &row.target_mib, &row.live, &row.peak, &row.utilisation);
    // NOLINTEND(bugprone-unchecked-string-to-number-conversion)
    row.found = fields == 6;
    return row;
}

}  // namespace

CY_TEST_CASE("samples/01-headless-host loads, simulates, reports its budgets and exits cleanly") {
    cy::test::TempDir content{"smoke_headless_host"};
    CY_REQUIRE(content.valid());

    const ProcessResult result = run_sample(content);
    CY_REQUIRE(result.ran);
    CY_CHECK_EQ(result.exit_code, 0);
    CY_CHECK(contains(result.output, "exit 0 (clean)"));

    // Every phase reported something, in the order the host brings its services up.
    CY_CHECK(contains(result.output, "startup  memory reflect jobs async vfs assets"));
    CY_CHECK(contains(result.output, "shutdown assets vfs async jobs reflect memory"));

    // The package really was cooked, mounted and read back: 4096 entities as two reflected records
    // each is 8192 records, and a decode that silently read nothing would report zero.
    CY_CHECK(contains(result.output, "package  entries=8 chunks=8"));
    CY_CHECK(contains(result.output, "records=8192"));

    // The parallelism was derived from the access declarations rather than declared: two writers of
    // different components share a batch, and the reader of both is ordered after them.
    CY_CHECK(contains(result.output, "schedule systems=3 batches=2  [decay drift] [summarise]"));

    // The control-plane rule held in this configuration, whichever one it is: the counter is a
    // plain atomic compiled into all four, so this assertion is not vacuous in Profile or Shipping.
    CY_CHECK(contains(result.output, "0 reflected lookups inside a hot region"));

    // The budget report: a real target against a real figure, for a domain that was actually used.
    const BudgetRow engine = budget_row(result.output, "engine");
    const BudgetRow ecs = budget_row(result.output, "ecs");
    CY_REQUIRE(engine.found);
    CY_REQUIRE(ecs.found);
    CY_CHECK_EQ(engine.target_mib, 4096ull);  // the desktop profile's root apportionment
    CY_CHECK_EQ(ecs.target_mib, 512ull);
    CY_CHECK(std::string{engine.kind} == "hard");
    CY_CHECK(std::string{ecs.kind} == "soft");
    // The entity arrays are the ECS domain's, so a zero here means the attribution was lost.
    CY_CHECK(ecs.live > 0);
    CY_CHECK(ecs.peak >= ecs.live);
    CY_CHECK(ecs.utilisation > 0.0);
    CY_CHECK(ecs.utilisation < 100.0);
    CY_CHECK(engine.live > 0);
    CY_CHECK_FALSE(contains(result.output, "OVER BUDGET"));
}

CY_TEST_CASE("samples/01-headless-host rejects a command line it does not understand") {
    cy::test::TempDir content{"smoke_headless_host_usage"};
    CY_REQUIRE(content.valid());

    // An unrecognised engine switch, and an undeclared setting. A dotted key on the command line is
    // a setting, and an unknown one is reported rather than ignored — that is the whole reason the
    // configuration store distinguishes the two.
    CY_CHECK_EQ(run_sample(content, "--no-such-option").exit_code, 2);
    CY_CHECK_EQ(run_sample(content, "--host.no-such-setting=1").exit_code, 2);
}

CY_TEST_CASE("startup order, shutdown order and the simulation are identical across 100 runs") {
    constexpr int kRuns = 100;

    cy::test::TempDir content{"smoke_headless_host_order"};
    CY_REQUIRE(content.valid());

    const ProcessResult first = run_sample(content);
    CY_REQUIRE(first.ran);
    CY_REQUIRE_EQ(first.exit_code, 0);
    const std::string expected = without_budget_rows(first.output);
    CY_REQUIRE_FALSE(line_with(expected, "01-headless-host: startup ").empty());
    CY_REQUIRE_FALSE(line_with(expected, "01-headless-host: shutdown ").empty());
    CY_REQUIRE_FALSE(line_with(expected, "01-headless-host: checksum ").empty());
    // See the header: the jobs line is compared like every other, and the sample reports the one
    // task counter that is ordered against the wait rather than the one that races it.
    CY_REQUIRE_FALSE(line_with(expected, "01-headless-host: jobs ").empty());

    int identical = 1;
    for (int run = 1; run < kRuns; ++run) {
        const ProcessResult next = run_sample(content);
        CY_REQUIRE(next.ran);
        CY_REQUIRE_EQ(next.exit_code, 0);
        const std::string actual = without_budget_rows(next.output);
        if (actual == expected) {
            ++identical;
        } else {
            CY_TEST_FAIL_CHECK("run " << run << " differs.\n  first:\n"
                                      << expected << "  this:\n"
                                      << actual);
        }
    }

    CY_CHECK_EQ(identical, kRuns);
    CY_TEST_MESSAGE(identical << " of " << kRuns << " runs reported the same order and the same "
                              << "checksum:\n"
                              << line_with(expected, "01-headless-host: startup ") << "\n"
                              << line_with(expected, "01-headless-host: shutdown ") << "\n"
                              << line_with(expected, "01-headless-host: checksum "));
}
