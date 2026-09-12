// Deterministic derivation, region independence, and the execution domains. M10 tasks.md 4.1.
//
// `procedural-content-generation` — "Deterministic derivation": "Generation SHALL derive its
// randomness from stable inputs: world seed, generator identity, generator version, region
// identity, and input dataset hashes — using the random stream mechanism in
// `simulation-and-determinism`. A global sequential generator SHALL NOT be used. **Changing one
// region SHALL NOT alter the content of another.**"
//
// ================================================================================================
// THE PRECONDITION RUNS FIRST, AND IT IS NOT A FORMALITY
// ================================================================================================
//
// The spike compared two identical full runs BEFORE it printed a matrix, and exited 2 with
// "THE HARNESS IS BROKEN" on a disagreement — because every other number in the report is a
// comparison against a full regeneration, and a full regeneration that is not reproducible makes
// all of them meaningless. The first case here is that precondition. If it fails, nothing in
// `test_invalidation.cpp` means anything either.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL: `eval_scatter()`'s draws were keyed on a running
// counter rather than on the candidate's own slot — the shape a generator reaches for first, and
// the one `simulation-and-determinism` forbids in as many words. BOTH the precondition and the
// region-independence case went red, and the pair is the point: the precondition caught it because
// the counter spans generators, and region independence would have caught it even if the counter
// had been reset per run. `pcg_scale`'s provenance-stripping case went red alongside, for the same
// reason. The slot keying was then restored. Reported in this milestone's `verified_failing`.

#include <cy/test/test.h>

#include <cy/pcg/execute.h>

#include "fixtures.h"

using cy::pcg::DirtyCause;
using cy::pcg::ExecutionDomain;
using cy::pcg::FlatSpatialQuery;
using cy::pcg::GenerationContext;
using cy::pcg::GenerationWorld;
using cy::pcg::Generator;
using cy::pcg::RegionCoord;
using cy::pcg::RegionSet;
using cy::pcg::RunOutcome;
using cy::pcg::RunProgress;
namespace test = cy::pcg::test;

namespace {

constexpr cy::i32 kEdge = 8;  ///< 8 x 8 regions of 64 m: a 512 m world, 16 384 raster cells.
constexpr cy::u64 kSeed = 0xc0ffee'1234'5678ULL;

}  // namespace

CY_TEST_CASE("two full generations of one seed are identical, in output and in identity") {
    const FlatSpatialQuery surface;
    const GenerationContext context = test::context_of(kSeed, surface);

    GenerationWorld first(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> a = test::make_generator(first);
    CY_REQUIRE(a.has_value());
    CY_REQUIRE(a->generate_all(ExecutionDomain::Cook, context).has_value());

    GenerationWorld second(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> b = test::make_generator(second);
    CY_REQUIRE(b.has_value());
    CY_REQUIRE(b->generate_all(ExecutionDomain::Cook, context).has_value());

    // THE PRECONDITION. Nothing else in this module's suites means anything without it.
    CY_CHECK_EQ(test::regions_disagreeing(first, second), 0u);
    CY_CHECK_EQ(test::world_digest(first), test::world_digest(second));

    // And the world is not empty, which is the other half of a comparison meaning something: two
    // generators that produced nothing would also agree.
    cy::u64 instances = 0;
    for (cy::i32 z = 0; z < kEdge; ++z) {
        for (cy::i32 x = 0; x < kEdge; ++x) {
            const cy::pcg::RegionState* state = first.find(RegionCoord{x, z, 0});
            CY_REQUIRE(state != nullptr);
            instances += state->accepted.size();
        }
    }
    CY_CHECK_GT(instances, 0u);
}

CY_TEST_CASE("a different seed produces a different world") {
    const FlatSpatialQuery surface;
    GenerationWorld first(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> a = test::make_generator(first);
    CY_REQUIRE(a.has_value());
    CY_REQUIRE(
        a->generate_all(ExecutionDomain::Cook, test::context_of(kSeed, surface)).has_value());

    GenerationWorld second(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> b = test::make_generator(second);
    CY_REQUIRE(b.has_value());
    CY_REQUIRE(
        b->generate_all(ExecutionDomain::Cook, test::context_of(kSeed + 1, surface)).has_value());

    // A generator that ignored its seed would pass every other case in this file.
    CY_CHECK_NE(test::world_digest(first), test::world_digest(second));
}

CY_TEST_CASE("one region's edit leaves its distant neighbours untouched") {
    // "Changing one region SHALL NOT alter the content of another", and the scenario: "WHEN one
    // region's parameters change and it is regenerated THEN neighbouring regions' generated content
    // SHALL be unchanged."
    //
    // The stamp is small and its region is a corner, so the regions on the far side of the world
    // are not downstream of it by any path — which is the claim. Regions NEAR it legitimately
    // change: that is the transitive edge doing its job, and `test_invalidation.cpp` measures how
    // far.
    const FlatSpatialQuery surface;
    const cy::pcg::AuthoredStamp stamp = test::hill(0, 0, 20.0, 40.0F);

    GenerationWorld before(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> a = test::make_generator(before);
    CY_REQUIRE(a.has_value());
    CY_REQUIRE(
        a->generate_all(ExecutionDomain::Cook, test::context_of(kSeed, surface)).has_value());

    GenerationWorld after(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> b = test::make_generator(after);
    CY_REQUIRE(b.has_value());
    CY_REQUIRE(b->generate_all(ExecutionDomain::Cook,
                               test::context_of(kSeed, surface,
                                                cy::Span<const cy::pcg::AuthoredStamp>(&stamp, 1)))
                   .has_value());

    // The corner region itself moved.
    const cy::pcg::RegionState* edited_before = before.find(RegionCoord{0, 0, 0});
    const cy::pcg::RegionState* edited_after = after.find(RegionCoord{0, 0, 0});
    CY_REQUIRE(edited_before != nullptr);
    CY_REQUIRE(edited_after != nullptr);
    CY_CHECK_NE(edited_before->accepted.digest(), edited_after->accepted.digest());

    // The opposite corner did not. Water flows downhill from the stamp, never uphill into a region
    // three-quarters of the world away across a ridge, so a difference here would be a derivation
    // that reads something it did not declare.
    const RegionCoord far{kEdge - 1, kEdge - 1, 0};
    const cy::pcg::RegionState* far_before = before.find(far);
    const cy::pcg::RegionState* far_after = after.find(far);
    CY_REQUIRE(far_before != nullptr);
    CY_REQUIRE(far_after != nullptr);
    CY_CHECK_EQ(far_before->accepted.digest(), far_after->accepted.digest());
    // And its identities, separately: the spike measured a configuration whose output matched and
    // whose labels had all moved.
    CY_CHECK_EQ(test::region_identity_digest(*far_before),
                test::region_identity_digest(*far_after));
}

CY_TEST_CASE("a generator not declared for a domain will not run in it") {
    // "A generator not declared for a domain SHALL NOT run in it. A city generator intended for
    // cooking SHALL NOT be invocable at runtime by accident."
    const FlatSpatialQuery surface;
    GenerationWorld world(test::allocator(), test::extent_of(2));
    cy::Expected<Generator, cy::Error> generator = test::make_generator(world);
    CY_REQUIRE(generator.has_value());
    CY_CHECK(generator->program().may_run_in(ExecutionDomain::Cook));
    CY_CHECK_FALSE(generator->program().may_run_in(ExecutionDomain::Runtime));

    cy::Expected<RunProgress, cy::Error> refused =
        generator->generate_all(ExecutionDomain::Runtime, test::context_of(kSeed, surface));
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::PermissionDenied);
    // Nothing was generated: the refusal is before the run, not a run that is undone.
    CY_CHECK_EQ(world.resident(), 0u);
}

CY_TEST_CASE("a budgeted run in steps produces exactly what one unbudgeted run produces") {
    // "Generation at runtime SHALL be incremental and budgeted: scheduled through the task system,
    // cancellable ... A runtime generator SHALL NOT block simulation or presentation. "
    const FlatSpatialQuery surface;
    const GenerationContext context = test::context_of(kSeed, surface);

    GenerationWorld whole(test::allocator(), test::extent_of(4));
    cy::Expected<Generator, cy::Error> one = test::make_generator(whole);
    CY_REQUIRE(one.has_value());
    CY_REQUIRE(one->generate_all(ExecutionDomain::Cook, context).has_value());

    GenerationWorld stepped(test::allocator(), test::extent_of(4));
    cy::Expected<Generator, cy::Error> two = test::make_generator(stepped);
    CY_REQUIRE(two.has_value());
    RegionSet everything(test::allocator(), test::extent_of(4));
    CY_REQUIRE(everything.resize());
    for (cy::i32 z = 0; z < 4; ++z) {
        for (cy::i32 x = 0; x < 4; ++x) {
            CY_REQUIRE(everything.add(RegionCoord{x, z, 0}));
        }
    }
    CY_REQUIRE(two->begin(ExecutionDomain::Cook, context, everything, DirtyCause::ProgramChanged));

    cy::u32 steps = 0;
    RunOutcome outcome = RunOutcome::BudgetExhausted;
    while (outcome != RunOutcome::Complete && steps < 10000) {
        // Three regions per step: small enough that the run is suspended inside an iterative
        // stage's sweep, which is the case a resumable cursor has to get right.
        cy::Expected<RunProgress, cy::Error> progress = two->step(3);
        CY_REQUIRE(progress.has_value());
        outcome = progress->outcome;
        ++steps;
    }
    CY_REQUIRE(outcome == RunOutcome::Complete);
    CY_CHECK_GT(steps, 1u);
    // The whole point: budgeting changes WHEN work happens and not WHAT it produces.
    CY_CHECK_EQ(test::world_digest(whole), test::world_digest(stepped));
}

CY_TEST_CASE("a cancelled run keeps its place and resumes to the same world") {
    const FlatSpatialQuery surface;
    const GenerationContext context = test::context_of(kSeed, surface);

    GenerationWorld whole(test::allocator(), test::extent_of(4));
    cy::Expected<Generator, cy::Error> one = test::make_generator(whole);
    CY_REQUIRE(one.has_value());
    CY_REQUIRE(one->generate_all(ExecutionDomain::Cook, context).has_value());

    GenerationWorld interrupted(test::allocator(), test::extent_of(4));
    cy::Expected<Generator, cy::Error> two = test::make_generator(interrupted);
    CY_REQUIRE(two.has_value());
    RegionSet everything(test::allocator(), test::extent_of(4));
    CY_REQUIRE(everything.resize());
    for (cy::i32 z = 0; z < 4; ++z) {
        for (cy::i32 x = 0; x < 4; ++x) {
            CY_REQUIRE(everything.add(RegionCoord{x, z, 0}));
        }
    }
    CY_REQUIRE(two->begin(ExecutionDomain::Cook, context, everything, DirtyCause::ProgramChanged));
    CY_REQUIRE(two->step(2).has_value());
    two->cancel();
    cy::Expected<RunProgress, cy::Error> cancelled = two->step(1000);
    CY_REQUIRE(cancelled.has_value());
    CY_CHECK(cancelled->outcome == RunOutcome::Cancelled);
    CY_CHECK(two->running());

    // Resuming is a flag and not a re-plan: the dirty set, the stage and the cursor were never
    // thrown away, which is what makes cancelling cheap enough to do on any frame that runs short.
    two->resume();
    cy::Expected<RunProgress, cy::Error> resumed = two->step(0xFFFFFFFFU);
    CY_REQUIRE(resumed.has_value());
    CY_CHECK(resumed->outcome == RunOutcome::Complete);
    CY_CHECK_EQ(test::world_digest(whole), test::world_digest(interrupted));
}

CY_TEST_CASE("a partial regeneration over a world that was never fully generated is refused") {
    // A gather would read a region with no values and would produce a result no full regeneration
    // could reproduce. Refused by name rather than answered plausibly.
    const FlatSpatialQuery surface;
    GenerationWorld world(test::allocator(), test::extent_of(4));
    cy::Expected<Generator, cy::Error> generator = test::make_generator(world);
    CY_REQUIRE(generator.has_value());

    RegionSet one(test::allocator(), test::extent_of(4));
    CY_REQUIRE(one.resize());
    CY_REQUIRE(one.add(RegionCoord{1, 1, 0}));
    cy::Expected<RunProgress, cy::Error> refused = generator->regenerate(
        ExecutionDomain::Cook, test::context_of(kSeed, surface), one, DirtyCause::AuthoredEdit);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::Unavailable);
}
