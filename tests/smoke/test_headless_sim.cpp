// The M2 milestone gate, as a test. Task 5.3.
//
// It runs samples/02-headless-sim — the artefact that authors a scene, cooks it into archetype
// blocks, loads it into a world and ticks it 10,000 fixed steps — and asserts the three things a
// milestone gate can assert about a whole process.
//
// FOUR CASES, AND WHAT EACH ONE WOULD CATCH.
//
//   The first reads the run's own report. The flattening decided what the specification says it
//   decides (two relationships baked, four kept), the cook emitted a reference site for the one
//   entity field the prefab declares, the resolve applied the override and the parameter, the world
//   holds the entities the cook said it would, and the node façade's world transform is the
//   entity's own component rather than a copy of it. A run that cooked nothing, flattened
//   everything, or silently dropped the reference would pass a test that only checked the exit
//   code.
//
//   The second runs it repeatedly, in separate processes, and requires every line of the report to
//   be identical. Separate processes rather than a loop: a fresh address space each time is what
//   would expose a hash that depended on an allocation address, a container's iteration order or a
//   per-process hash seed, and a hundred iterations of one loop would reproduce such a hash
//   faithfully and call it deterministic. It is the same argument
//   tests/smoke/test_headless_host.cpp makes about the startup sequence, applied to the state of a
//   simulated world.
//
//   The third is the snapshot claim, read off the run's own numbers: the world hashed one way,
//   moved when it was ticked further, and hashed the same way again once the snapshot taken before
//   those ticks was restored. Both halves matter — a restore that reproduced a world which had
//   never been allowed to change would prove nothing at all, so the test requires the intervening
//   hash to differ.
//
//   The fourth requires a different session seed to produce a different hash while producing the
//   same content. Every random draw in the sample is keyed by the seed, so a hash that ignored it
//   would mean the seeded streams were not reaching the simulation — which is a failure that looks
//   exactly like success in the first three cases.
//
// NOTHING IS EXCLUDED FROM THE COMPARISON, and that is a difference from the M1 gate rather than an
// oversight. `test_headless_host.cpp` has to exclude its budget rows, because a domain's peak is a
// high-water mark over concurrent allocations. This sample prints no such figure: every number in
// its report is a function of the content, and if one ever is not, the right fix is to stop
// printing it rather than to stop comparing it.

#include <cy/test/test.h>

#include <cstdio>
#include <string>

#include "process.h"

namespace {

using cy::test::smoke::ProcessResult;

/// Run the sample. `extra` is appended to the command line.
ProcessResult run_sample(const char* extra = "") {
    return cy::test::smoke::run(cy::test::smoke::quoted(CY_SAMPLE_HEADLESS_SIM) + " " + extra);
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

/// The sixteen hex digits following `label` in `text`, as a number. Zero when absent — and zero is
/// not a hash this sample can produce, so a caller comparing against another one is safe.
unsigned long long hash_after(const std::string& text, const char* label) {
    const std::string::size_type at = text.find(label);
    if (at == std::string::npos) {
        return 0;
    }
    unsigned long long value = 0;
    // sscanf, and its result is checked: one conversion, and `fields == 1` is exactly the failure
    // this reads for.
    // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion)
    const int fields = std::sscanf(text.c_str() + at + std::string{label}.size(), "%16llx", &value);
    return fields == 1 ? value : 0;
}

}  // namespace

CY_TEST_CASE("samples/02-headless-sim authors, cooks, ticks and exits cleanly") {
    const ProcessResult result = run_sample();
    CY_REQUIRE(result.ran);
    CY_CHECK_EQ(result.exit_code, 0);
    CY_CHECK(contains(result.output, "exit 0 (clean)"));

    // The authoring documents really were written and read back: four prefab entities, two
    // placements of them, one exposed parameter.
    CY_CHECK(contains(result.output, "authored prefab=4 entities  placements=2  parameters=1"));

    // The resolve collapsed the composition: eight entities from two placements of a four-entity
    // prefab, the one override applied, and the one parameter applied to two fields per placement.
    CY_CHECK(contains(result.output, "resolved entities=8 overrides=1 parameters=4 conflicts=0"));

    // THE FLATTENING RULE, as a number. The skirt's relationship goes because nothing above it ever
    // moves; the muzzle's stays because the yaw above it does, even though the muzzle itself never
    // moves relative to its parent. A per-edge test would report four flattened and three retained,
    // and would have welded the muzzle to a turret that turns.
    CY_CHECK(contains(result.output, "flattened=2"));
    CY_CHECK(contains(result.output, "retained=4"));
    // One entity field is declared in the prefab, so the cook emits exactly one reference site, and
    // it points inside the template rather than outside it.
    CY_CHECK(contains(result.output, "references=1 dangling=0"));

    // The cooked blocks became entities in this world's chunks.
    CY_CHECK(contains(result.output, "world    instances=64 entities=530"));
    CY_CHECK(contains(result.output, "nodes    scene=1 nodes=16 batteries=3 turrets=4 systems=2"));

    // The loop ran the fixed steps it was asked for: 2,500 frames of four ticks each.
    CY_CHECK(contains(result.output, "tick     frames=2500 ticks=10000"));

    // The hash covered the entities the world holds, and says how much of the world it is silent
    // about rather than reporting a healthy-looking number over a tenth of the state.
    CY_CHECK(contains(result.output, "entities=530"));
    CY_CHECK(contains(result.output, "schema   subjects declared=4"));
    // The hierarchy is a hierarchy: a world node with archetypes under it, not one number.
    CY_CHECK(contains(result.output, "level    world"));
    CY_CHECK(contains(result.output, "level      archetype"));

    // THE COHERENCE INVARIANT: a node's world transform is the entity's component, exactly.
    CY_CHECK(contains(result.output, "reads the entity's own transform: yes"));
}

CY_TEST_CASE("the state hash is identical across processes") {
    constexpr int kRuns = 20;

    const ProcessResult first = run_sample();
    CY_REQUIRE(first.ran);
    CY_REQUIRE_EQ(first.exit_code, 0);
    CY_REQUIRE_FALSE(line_with(first.output, "02-headless-sim: hash ").empty());

    int identical = 1;
    for (int run = 1; run < kRuns; ++run) {
        const ProcessResult next = run_sample();
        CY_REQUIRE(next.ran);
        CY_REQUIRE_EQ(next.exit_code, 0);
        if (next.output == first.output) {
            ++identical;
        } else {
            CY_TEST_FAIL_CHECK("run " << run << " differs.\n  first:\n"
                                      << first.output << "  this:\n"
                                      << next.output);
        }
    }

    CY_CHECK_EQ(identical, kRuns);
    CY_TEST_MESSAGE(identical << " of " << kRuns << " processes reported the same state:\n"
                              << line_with(first.output, "02-headless-sim: hash "));
}

CY_TEST_CASE("the state hash reproduces after a snapshot restore, and the snapshot was needed") {
    const ProcessResult result = run_sample();
    CY_REQUIRE(result.ran);
    CY_REQUIRE_EQ(result.exit_code, 0);

    const std::string restore = line_with(result.output, "02-headless-sim: restore  settled=");
    CY_REQUIRE_FALSE(restore.empty());
    const unsigned long long settled = hash_after(restore, "settled=");
    const unsigned long long diverged = hash_after(restore, "ticks=");
    const unsigned long long restored = hash_after(restore, "restored=");

    CY_CHECK(settled != 0);
    CY_CHECK_EQ(settled, restored);
    // The world was allowed to change in between. Without this the case would pass on a restore
    // that did nothing, over a world that could not have moved anyway.
    CY_CHECK(diverged != settled);
    CY_CHECK(contains(result.output, "the world moved: yes   the restore reproduced it: yes"));

    // The hash the run reported is the one the restore was checked against, rather than a second
    // number taken somewhere else.
    CY_CHECK_EQ(hash_after(result.output, "02-headless-sim: hash     "), settled);
}

CY_TEST_CASE("a different session seed is a different simulation, over the same content") {
    const ProcessResult first = run_sample();
    const ProcessResult second = run_sample("--seed 7");
    CY_REQUIRE(first.ran);
    CY_REQUIRE(second.ran);
    CY_REQUIRE_EQ(first.exit_code, 0);
    CY_REQUIRE_EQ(second.exit_code, 0);

    // Same content: the authoring text is byte-identical, so the digest is.
    CY_CHECK_EQ(line_with(first.output, "02-headless-sim: authored "),
                line_with(second.output, "02-headless-sim: authored "));
    CY_CHECK_EQ(line_with(first.output, "02-headless-sim: cooked "),
                line_with(second.output, "02-headless-sim: cooked "));
    // Different simulation: every draw is keyed by the seed.
    CY_CHECK(hash_after(first.output, "02-headless-sim: hash     ") !=
             hash_after(second.output, "02-headless-sim: hash     "));
}

CY_TEST_CASE("samples/02-headless-sim rejects a command line it does not understand") {
    CY_CHECK_EQ(run_sample("--no-such-option").exit_code, 2);
    // A tick cap above the eight `engine-architecture` fixes is refused rather than clamped: the
    // cap exists so the loop cannot enter a death spiral, and silently accepting nine would be a
    // configuration nobody could reproduce from the command line they typed.
    CY_CHECK_EQ(run_sample("--ticks-per-frame 9").exit_code, 2);
}
