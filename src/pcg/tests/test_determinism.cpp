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

#include <cmath>

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

// ================================================================================================
// THE M10 GATE'S ADVERSARIAL PASS (tasks.md 9.3): TRYING TO MAKE ONE SEED PRODUCE TWO WORLDS
// ================================================================================================
//
// The cases above hold the precondition and the region independence. These attack the three ways a
// second run of one seed could differ that none of them covers: a different traversal order, a
// second run inside one process on state the first run left behind, and a region whose DETAIL was
// dropped between the two — which is the residency move `procedural-content-generation` calls
// demotion, and which a partial regeneration afterwards must either reproduce or refuse.

CY_TEST_CASE("a second full generation on one generator, in one process, reproduces the first") {
    // THE ATTACK. A `Generator` carries a run identifier, two ledgers, a cursor and a dirty set
    // across runs. A second `generate_all()` on the same object is the shape a cooker has when it
    // re-cooks, and anything the first run left behind that the second reads is a world that
    // depends on its own history rather than on its seed.
    const FlatSpatialQuery surface;
    const GenerationContext context = test::context_of(kSeed, surface);

    GenerationWorld reference(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> once = test::make_generator(reference);
    CY_REQUIRE(once.has_value());
    CY_REQUIRE(once->generate_all(ExecutionDomain::Cook, context).has_value());

    GenerationWorld world(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> twice = test::make_generator(world);
    CY_REQUIRE(twice.has_value());
    CY_REQUIRE(twice->generate_all(ExecutionDomain::Cook, context).has_value());
    const cy::u64 after_first = test::world_digest(world);
    CY_REQUIRE(twice->generate_all(ExecutionDomain::Cook, context).has_value());

    CY_CHECK_EQ(test::world_digest(world), after_first);
    CY_CHECK_EQ(test::regions_disagreeing(reference, world), 0u);
}

CY_TEST_CASE("a dirty set seeded in the reverse order regenerates to the same world") {
    // THE ATTACK. `RegionSet::members()` promises a canonical order regardless of insertion order,
    // and every claim about a partial regeneration rests on that promise. It is asserted here by
    // running the same regeneration twice with the seeds inserted in opposite orders, because an
    // order that leaked would show up as a different world rather than as a different listing.
    const FlatSpatialQuery surface;
    const GenerationContext context = test::context_of(kSeed, surface);

    const RegionCoord seeds_in_order[4] = {RegionCoord{2, 2, 0}, RegionCoord{3, 2, 0},
                                           RegionCoord{2, 3, 0}, RegionCoord{3, 3, 0}};

    cy::u64 digests[2] = {0, 0};
    for (cy::usize pass = 0; pass < 2; ++pass) {
        GenerationWorld world(test::allocator(), test::extent_of(kEdge));
        cy::Expected<Generator, cy::Error> generator = test::make_generator(world);
        CY_REQUIRE(generator.has_value());
        CY_REQUIRE(generator->generate_all(ExecutionDomain::Cook, context).has_value());

        RegionSet dirty(test::allocator(), test::extent_of(kEdge));
        CY_REQUIRE(dirty.resize());
        for (cy::usize index = 0; index < 4; ++index) {
            const cy::usize at = pass == 0 ? index : 3 - index;
            CY_REQUIRE(dirty.add(seeds_in_order[at]));
        }
        CY_REQUIRE(
            generator
                ->regenerate(ExecutionDomain::Cook, context, dirty, DirtyCause::UpstreamStage, 0)
                .has_value());
        digests[pass] = test::world_digest(world);
    }
    CY_CHECK_EQ(digests[0], digests[1]);
}

CY_TEST_CASE("a partial regeneration beside a DEMOTED region reproduces the full run, or refuses") {
    // THE ATTACK, and it is the one the brief calls "a different residency level". `demote()` drops
    // a region's detail and keeps its macro summary — the round trip the specification asks for.
    // What it does NOT drop is the region's `evaluated_mask`, so `complete_through()` still calls
    // the world complete and a partial regeneration afterwards is not refused.
    //
    // `forest.spacing` resolves candidate conflicts ACROSS REGION BOUNDARIES by reading its
    // neighbours' CANDIDATES, which demotion cleared. So a region beside a demoted one, regenerated
    // from the same seed with nothing else changed, is evaluated against a neighbour that now
    // supplies no candidates at all — and the result would be a world no full regeneration of this
    // seed could produce.
    //
    // THE PAIR IS FOUND RATHER THAN ASSUMED. A demotion beside a boundary where nothing contends
    // proves nothing, and this project has already shipped a determinism test whose scene never
    // contended. The region demoted below is one whose candidates DEMONSTRABLY beat a neighbour's
    // in the reference run, and the case refuses to proceed without such a pair.
    const FlatSpatialQuery surface;
    const GenerationContext context = test::context_of(kSeed, surface);

    GenerationWorld reference(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> whole = test::make_generator(reference);
    CY_REQUIRE(whole.has_value());
    CY_REQUIRE(whole->generate_all(ExecutionDomain::Cook, context).has_value());

    RegionCoord demoted{0, 0, 0};
    RegionCoord neighbour{0, 0, 0};
    bool found = false;
    for (cy::i32 z = 0; z < kEdge && !found; ++z) {
        for (cy::i32 x = 0; x < kEdge && !found; ++x) {
            const cy::pcg::RegionState* state = reference.find(RegionCoord{x, z, 0});
            if (state == nullptr) {
                continue;
            }
            for (const cy::pcg::RejectionRecord& record : state->provenance.rejected()) {
                if (!record.beaten_by.is_valid() || record.beaten_in_region == record.region) {
                    continue;  // a threshold rejection, or a conflict inside one region
                }
                // `beaten_in_region` holds the candidate that WON. Demoting it takes the winner's
                // candidate list away from the region that lost to it.
                neighbour = RegionCoord{x, z, 0};
                demoted = record.beaten_in_region;
                found = demoted.x != neighbour.x || demoted.z != neighbour.z;
            }
        }
    }
    CY_REQUIRE(found);  // no cross-region conflict: this case would prove nothing

    GenerationWorld world(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> generator = test::make_generator(world);
    CY_REQUIRE(generator.has_value());
    CY_REQUIRE(generator->generate_all(ExecutionDomain::Cook, context).has_value());
    CY_REQUIRE_EQ(test::regions_disagreeing(reference, world), 0u);

    const cy::pcg::RegionState* expected = reference.find(neighbour);
    CY_REQUIRE(expected != nullptr);
    CY_REQUIRE(expected->accepted.size() > 0);
    const cy::u64 expected_digest = expected->accepted.digest();

    CY_REQUIRE(world.demote(demoted).has_value());
    const cy::pcg::RegionState* dropped = world.find(demoted);
    CY_REQUIRE(dropped != nullptr);
    CY_REQUIRE_EQ(dropped->candidates.size(), 0u);

    const cy::u8 spacing = generator->stage_of(cy::pcg::node_identity(test::kSpacingNode));
    CY_REQUIRE(spacing != cy::pcg::Program::kNoStage);

    RegionSet dirty(test::allocator(), test::extent_of(kEdge));
    CY_REQUIRE(dirty.resize());
    CY_REQUIRE(dirty.add(neighbour));

    const cy::Expected<RunProgress, cy::Error> run = generator->regenerate(
        ExecutionDomain::Cook, context, dirty, DirtyCause::UpstreamStage, spacing);
    if (!run) {
        // A refusal is the correct answer, and it is the one this module gives: the world it was
        // asked to regenerate over has a hole in it, and a gather would read a region with no
        // values. The other branch is left standing because a future promote-on-read would make it
        // the right one, and the claim is about the OUTCOME rather than about which way it is met.
        CY_CHECK(run.error().code == cy::ErrorCode::Unavailable);
        return;
    }
    const cy::pcg::RegionState* actual = world.find(neighbour);
    CY_REQUIRE(actual != nullptr);
    CY_CHECK_EQ(actual->accepted.digest(), expected_digest);
}

CY_TEST_CASE(
    "the BUDGETED entry point refuses an incomplete world too, not only the blocking one") {
    // THE ATTACK. `regenerate()` refuses a partial regeneration over a world that was never fully
    // generated, and `begin()` — the budgeted, resumable, RUNTIME path, the one a game actually
    // calls — did not: it went straight to work and `step()` drove it. A refusal that only the
    // cook-time entry point makes is a refusal the runtime does not have.
    const FlatSpatialQuery surface;
    const GenerationContext context = test::context_of(kSeed, surface);

    GenerationWorld world(test::allocator(), test::extent_of(4));
    cy::Expected<Generator, cy::Error> generator = test::make_generator(world);
    CY_REQUIRE(generator.has_value());

    RegionSet one(test::allocator(), test::extent_of(4));
    CY_REQUIRE(one.resize());
    CY_REQUIRE(one.add(RegionCoord{1, 1, 0}));
    const cy::Status began =
        generator->begin(ExecutionDomain::Cook, context, one, DirtyCause::UpstreamStage);
    CY_REQUIRE_FALSE(began.has_value());
    CY_CHECK(began.error().code == cy::ErrorCode::Unavailable);

    // An edit over a world that was never generated is always partial, and is refused the same way.
    const cy::pcg::AuthoredStamp stamp = test::hill(1, 1);
    CY_CHECK_FALSE(generator
                       ->begin_edit(ExecutionDomain::Cook, context,
                                    cy::Span<const cy::pcg::AuthoredStamp>(&stamp, 1))
                       .has_value());

    // And the run that FILLS the world is still allowed to be budgeted: seeding every region from
    // stage zero reads nothing it does not also write.
    RegionSet everything(test::allocator(), test::extent_of(4));
    CY_REQUIRE(everything.resize());
    for (cy::i32 z = 0; z < 4; ++z) {
        for (cy::i32 x = 0; x < 4; ++x) {
            CY_REQUIRE(everything.add(RegionCoord{x, z, 0}));
        }
    }
    CY_REQUIRE(
        generator->begin(ExecutionDomain::Cook, context, everything, DirtyCause::ProgramChanged));
    cy::Expected<RunProgress, cy::Error> progress = generator->step(0xFFFFFFFFU);
    CY_REQUIRE(progress.has_value());
    CY_CHECK(progress->outcome == RunOutcome::Complete);
}

CY_TEST_CASE("a world far from the origin generates values, not a wrapped hash subject") {
    // REGRESSION for a latent defect found by the clang-tidy sweep and confirmed by hand
    // (bugprone-misplaced-widening-cast at src/pcg/src/execute.cpp). `lattice_value()` biased the
    // lattice coordinate by 2^20 before widening it to the u64 the counter-based stream is keyed
    // on, and the addition was therefore performed in `int`. For any lattice coordinate above
    // INT_MAX - 2^20 that is SIGNED OVERFLOW — undefined behaviour — in the function that feeds the
    // noise hash, and where it wrapped the stream was keyed on 18446744071562132896 instead of
    // 2147548576. A different subject is a different height for that cell.
    //
    // THE ORIGIN BELOW IS NOT ARBITRARY. The noise node draws three octaves at 1/96, 1/48 and 1/24,
    // and the lattice coordinate is floor(world_x * frequency). 51 516 000 000 m puts the THIRD
    // octave at 2 146 500 000 — past the INT_MAX - 2^20 = 2 146 435 071 where the `int` form
    // overflows — while leaving the first two, and the `f64` -> `i32` cast every octave makes, well
    // inside range. Move the constant and the case stops covering anything.
    //
    // WHAT MAKES IT RED. Under `just test-sanitize --sanitizer undefined` the pre-fix evaluator
    // aborts in this case and in no other, because no other case in the module reaches the band.
    // An ordinary build cannot see the difference — the wrapped key is still a key, so the run is
    // still self-consistent — which is precisely why the band needed a case of its own rather than
    // a wider assertion somewhere else.
    constexpr cy::f64 kFarOrigin = 51'516'000'000.0;
    constexpr cy::i32 kFarEdge = 2;

    const FlatSpatialQuery surface;
    GenerationContext context = test::context_of(kSeed, surface);
    context.origin_x = kFarOrigin;
    context.origin_z = kFarOrigin;

    GenerationWorld first(test::allocator(), test::extent_of(kFarEdge));
    cy::Expected<Generator, cy::Error> a = test::make_generator(first);
    CY_REQUIRE(a.has_value());
    CY_REQUIRE(a->generate_all(ExecutionDomain::Cook, context).has_value());

    GenerationWorld second(test::allocator(), test::extent_of(kFarEdge));
    cy::Expected<Generator, cy::Error> b = test::make_generator(second);
    CY_REQUIRE(b.has_value());
    CY_REQUIRE(b->generate_all(ExecutionDomain::Cook, context).has_value());

    CY_CHECK_EQ(test::regions_disagreeing(first, second), 0u);
    CY_CHECK_EQ(test::world_digest(first), test::world_digest(second));

    // And the result is a height field rather than garbage: every cell finite and inside the
    // amplitude the graph declared, summed over three octaves that halve.
    const cy::pcg::RegionState* state = first.find(RegionCoord{0, 0, 0});
    CY_REQUIRE(state != nullptr);
    cy::pcg::AttributeId noise_output;
    for (const cy::pcg::Stage& stage : a->program().stages()) {
        if (stage.kind == cy::pcg::NodeKind::Noise) {
            noise_output = stage.output;
        }
    }
    CY_REQUIRE(noise_output.is_valid());
    const cy::Span<const cy::f32> height = state->raster.values(noise_output);
    CY_REQUIRE_FALSE(height.empty());
    for (const cy::f32 value : height) {
        CY_CHECK(std::isfinite(value));
        CY_CHECK(value >= 0.0F);
        // 48 + 24 + 12 = 84 is the sum of the three octave amplitudes. The bound is loose on
        // purpose: the claim is "a height field", not a re-derivation of the spectrum.
        CY_CHECK(value <= 96.0F);
    }
}
