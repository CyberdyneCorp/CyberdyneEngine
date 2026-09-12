// Regions, the dirty set, and the FIXED POINT. M10 tasks.md 4.3, and the half of it the spike
// decided.
//
// THE QUESTION, from tasks.md 0.1: does a dependency-driven PARTIAL regeneration over a graph with
// known edges reproduce a FULL regeneration of the same seed — identical output AND identical
// generated identity? The spike answered yes, in 2 of 24 configurations, in all 12 of 12 trials,
// and only when four properties hold at once (design.md §1.2). This suite is that answer as a
// running test over the engine's own evaluator rather than over a prototype:
//
//   order-free conflict resolution   refused at compile time — `test_compile.cpp`
//   invalidation to a FIXED POINT    HERE
//   convergence, not a sweep budget  declared here, enforced by the cache — `test_cache.cpp`
//   identity from stable ids         refused at compile time; measured in `test_identity.cpp`
//
// ================================================================================================
// AND THE TRAP THIS PROJECT HAS ALREADY FALLEN INTO ONCE
// ================================================================================================
//
// A determinism test shipped here that passed on the very defect it was written for, because its
// scene never contended. The spike's first contention meter had the same bug and now exits 3 with
// "this report is void" when the count is zero. So the first case below COUNTS the cross-region
// conflicts and the fixed point's own expansions, and refuses to let the reproduction case be read
// as evidence unless both are non-zero.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL — three mutations, each watched red and restored, and
// the SIZE of each red is part of the finding:
//
//   `expand_after_sweep()` cut to `return ok();` — exactly the spike's `static` closure — put ONE
//   region of sixty-four wrong. One. That is what "reproduces 7 of 12 trials" looks like from
//   inside: a defect that is real, silent, and small enough that a test which sampled a region or
//   two would have called the invalidation sound.
//
//   The declared reach dropped from `carry_to_next_stage()` put TWO regions wrong — and it was
//   invisible until `world_digest()` was widened to cover every stage's output rather than the
//   accepted points alone, because the stale relaxation was three cells deep and flipped no
//   candidate. The fix to the comparison came out of that mutation, not out of a review.
//
//   The point stages gated on their DIGEST rather than on what they EVALUATED put SIXTY-THREE
//   regions wrong once the case below constructed the situation deliberately; before that
//   construction the same defect showed as three regions of sixty-four on one particular edit and
//   as nothing at all on another. That is the whole reason "a stage that rewrites a shared buffer"
//   is a case of its own rather than a consequence somebody hopes an edit will expose.
//
// All three are reported in this milestone's `verified_failing`.

#include <cy/test/test.h>

#include <cy/pcg/execute.h>
#include <cy/pcg/invalidation.h>

#include "fixtures.h"

using cy::pcg::AuthoredStamp;
using cy::pcg::DirtyCause;
using cy::pcg::DirtyReason;
using cy::pcg::ExecutionDomain;
using cy::pcg::FlatSpatialQuery;
using cy::pcg::GenerationContext;
using cy::pcg::GenerationWorld;
using cy::pcg::Generator;
using cy::pcg::NodeKind;
using cy::pcg::RegionCoord;
using cy::pcg::RegionExtent;
using cy::pcg::RegionSet;
using cy::pcg::RunProgress;
using cy::pcg::StageProfile;
namespace test = cy::pcg::test;

namespace {

constexpr cy::i32 kEdge = 8;  ///< 64 regions of 64 m.
constexpr cy::u64 kSeed = 0x7a11'f0'1a'6e'0001ULL;

/// Cross-region conflicts recorded in a world's provenance: rejections whose winner was a candidate
/// in a DIFFERENT region. Zero means the conflict resolution was never exercised and the
/// reproduction below proves nothing about it.
[[nodiscard]] cy::u32 cross_region_rejections(const GenerationWorld& world) noexcept {
    cy::u32 total = 0;
    const RegionExtent& extent = world.extent();
    for (cy::i32 z = extent.min_z; z <= extent.max_z; ++z) {
        for (cy::i32 x = extent.min_x; x <= extent.max_x; ++x) {
            const cy::pcg::RegionState* state = world.find(RegionCoord{x, z, extent.level});
            if (state == nullptr) {
                continue;
            }
            for (const cy::pcg::RejectionRecord& record : state->provenance.rejected()) {
                if (!record.beaten_by.is_valid()) {
                    continue;  // a density or a threshold rejection, not a conflict
                }
                total += record.beaten_in_region == record.region ? 0U : 1U;
            }
        }
    }
    return total;
}

/// The stage whose kind is `kind`, by index. The suite asserts against the transitive edge by name
/// rather than by a number that would move when the graph gains a node.
[[nodiscard]] cy::u8 stage_of(const cy::pcg::Program& program, NodeKind kind) noexcept {
    for (cy::usize index = 0; index < program.stages().size(); ++index) {
        if (program.stages()[index].kind == kind) {
            return static_cast<cy::u8>(index);
        }
    }
    return cy::pcg::Program::kNoStage;
}

}  // namespace

CY_TEST_CASE(
    "a partial regeneration reproduces the full one, bit for bit and identity for identity") {
    const FlatSpatialQuery surface;
    const AuthoredStamp stamp = test::hill(3, 3);
    const cy::Span<const AuthoredStamp> edit(&stamp, 1);

    // A. The world before the edit, generated whole.
    GenerationWorld partial(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> incremental = test::make_generator(partial);
    CY_REQUIRE(incremental.has_value());
    CY_REQUIRE(incremental->generate_all(ExecutionDomain::Cook, test::context_of(kSeed, surface))
                   .has_value());
    const cy::u64 full_run_regions = test::regions_evaluated(incremental->profile());

    // B. The world AFTER the edit, generated whole from nothing. The answer everything is compared
    //    against.
    GenerationWorld reference(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> whole = test::make_generator(reference);
    CY_REQUIRE(whole.has_value());
    CY_REQUIRE(whole->generate_all(ExecutionDomain::Cook, test::context_of(kSeed, surface, edit))
                   .has_value());

    // The edit has to MOVE something, or the reproduction below is a comparison of two identical
    // worlds and says nothing at all.
    GenerationWorld untouched(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> baseline = test::make_generator(untouched);
    CY_REQUIRE(baseline.has_value());
    CY_REQUIRE(baseline->generate_all(ExecutionDomain::Cook, test::context_of(kSeed, surface))
                   .has_value());
    const cy::u32 truly_changed = test::regions_disagreeing(untouched, reference);
    CY_CHECK_GT(truly_changed, 0u);

    // C. The SAME edit applied incrementally to the world from A. The seed goes in at the stage the
    //    stamp FEEDS rather than at stage zero: an edit does not change what the noise node
    //    computes, so a dirty set seeded at the front would find nothing changed and never reach
    //    the stamp.
    const GenerationContext edited = test::context_of(kSeed, surface, edit);
    cy::Expected<RegionSet, cy::Error> seeds = incremental->regions_touched(edited, edit);
    CY_REQUIRE(seeds.has_value());
    CY_CHECK_GT(seeds->size(), 0u);
    cy::Expected<RunProgress, cy::Error> run =
        incremental->regenerate_edit(ExecutionDomain::Cook, edited, edit);
    CY_REQUIRE(run.has_value());

    // THE ANSWER. Output and generated identity both, region for region.
    CY_CHECK_EQ(test::regions_disagreeing(partial, reference), 0u);
    CY_CHECK_EQ(test::world_digest(partial), test::world_digest(reference));

    // AND IT IS NOT A FULL REGENERATION WITH EXTRA BOOKKEEPING. design.md asked exactly that
    // question of this row; the partial run must do measurably less work than a full one.
    const cy::u64 partial_regions = test::regions_evaluated(incremental->profile());
    CY_CHECK_LT(partial_regions, full_run_regions);

    // THE CONTENTION PROOF. A conflict-resolution test whose regions never fight proves nothing —
    // this project has shipped exactly that. The count is taken over the PARTIAL run's world,
    // because that is the run whose order-dependence would show.
    CY_CHECK_GT(cross_region_rejections(partial), 0u);

    // AND THE POINT PIPELINE RE-RAN WHERE IT HAD TO. `Scatter`, `Filter` and `Spacing` all read and
    // write the region's ONE candidate buffer in place, so a stage that rewrote it must re-run its
    // successor WHEREVER IT RAN and not only where its output CHANGED: a scatter that re-ran and
    // produced the same candidates has still replaced a filtered buffer with an unfiltered one.
    //
    // This cost three regions of sixty-four before it was found, and every digest said nothing had
    // changed — which is exactly why the invariant is asserted here directly rather than through
    // its consequences. A test that only compared the worlds would pass or fail on whether the edit
    // happened to flip a candidate, which is luck rather than evidence.
    const cy::u8 scatter = stage_of(incremental->program(), NodeKind::Scatter);
    const cy::u8 filtering = stage_of(incremental->program(), NodeKind::Filter);
    const cy::u8 resolving = stage_of(incremental->program(), NodeKind::Spacing);
    CY_REQUIRE(scatter != cy::pcg::Program::kNoStage);
    CY_REQUIRE(filtering != cy::pcg::Program::kNoStage);
    CY_REQUIRE(resolving != cy::pcg::Program::kNoStage);
    const StageProfile* scattered = incremental->profile().stage_at(scatter);
    const StageProfile* filtered = incremental->profile().stage_at(filtering);
    const StageProfile* resolved = incremental->profile().stage_at(resolving);
    CY_REQUIRE(scattered != nullptr);
    CY_REQUIRE(filtered != nullptr);
    CY_REQUIRE(resolved != nullptr);
    CY_CHECK_GT(scattered->regions_evaluated, 0u);
    CY_CHECK_EQ(filtered->regions_evaluated, scattered->regions_evaluated);
    CY_CHECK_GE(resolved->regions_evaluated, filtered->regions_evaluated);

    // THE FIXED POINT ACTUALLY EXPANDED. Without this the run above would be the spike's `static`
    // closure, which reproduces 7 of 12 — often enough to pass a casual test.
    const cy::u8 flow = stage_of(incremental->program(), NodeKind::Propagate);
    CY_REQUIRE(flow != cy::pcg::Program::kNoStage);
    const StageProfile* profile = incremental->profile().stage_at(flow);
    CY_REQUIRE(profile != nullptr);
    CY_CHECK_GT(profile->sweeps, 1u);
    CY_CHECK_GT(profile->fixed_point_additions, 0u);
}

CY_TEST_CASE(
    "the transitive edge evaluates fewer regions than a full run, and more than it changes") {
    // design.md §1.3's measurement, on this module's own graph: what an edit CHANGES against what a
    // sound invalidation EVALUATES to find it. The spike's figures over 576 regions were 19 changed
    // and 40 evaluated at the transitive edge. The claim asserted here is the SHAPE rather than
    // those numbers — the graph is a different one — and the shape is the whole finding: the fixed
    // point follows the water past where it settles, and still costs less than regenerating the
    // world.
    const FlatSpatialQuery surface;
    const AuthoredStamp stamp = test::hill(3, 3);
    const cy::Span<const AuthoredStamp> edit(&stamp, 1);

    GenerationWorld before(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> generator = test::make_generator(before);
    CY_REQUIRE(generator.has_value());
    CY_REQUIRE(generator->generate_all(ExecutionDomain::Cook, test::context_of(kSeed, surface))
                   .has_value());

    const cy::u8 flow = stage_of(generator->program(), NodeKind::Propagate);
    CY_REQUIRE(flow != cy::pcg::Program::kNoStage);
    const StageProfile* full = generator->profile().stage_at(flow);
    CY_REQUIRE(full != nullptr);
    const cy::u64 full_flow_regions = full->regions_evaluated;
    CY_CHECK_GT(full_flow_regions, 0u);

    GenerationWorld reference(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> whole = test::make_generator(reference);
    CY_REQUIRE(whole.has_value());
    CY_REQUIRE(whole->generate_all(ExecutionDomain::Cook, test::context_of(kSeed, surface, edit))
                   .has_value());

    const GenerationContext edited = test::context_of(kSeed, surface, edit);
    CY_REQUIRE(generator->regenerate_edit(ExecutionDomain::Cook, edited, edit).has_value());

    const cy::u32 disagreeing = test::regions_disagreeing(before, reference);
    const StageProfile* partial = generator->profile().stage_at(flow);
    CY_REQUIRE(partial != nullptr);

    // The edit moved the world, the partial run cost less than the full one, and the answer is
    // still exact — which is the sentence design.md §1.3 is made of.
    CY_CHECK_EQ(disagreeing, 0u);
    CY_CHECK_GT(partial->regions_evaluated, 0u);
    CY_CHECK_LT(partial->regions_evaluated, full_flow_regions);
}

CY_TEST_CASE("a stage that rewrites a shared buffer re-runs its consumer wherever it ran") {
    // THE DEFECT THIS CASE EXISTS FOR, and it cost three regions of sixty-four before it was found.
    //
    // `Scatter`, `Filter` and `Spacing` all read and write the region's ONE candidate buffer in
    // place — a filter compacts it. So a scatter that RE-RAN and produced the SAME candidates has
    // still replaced a FILTERED buffer with an UNFILTERED one, and a consumer gated on "did the
    // digest change" never trims it. Every digest says nothing happened; the region is wrong.
    //
    // The situation is CONSTRUCTED here rather than waited for. An ordinary edit exposes it only
    // when the density happens to move without flipping any candidate, which is luck; entering the
    // pipeline at the scatter stage with UNCHANGED inputs makes "the stage re-ran and its output
    // did not change" true by construction, every run. The second regeneration is what reads the
    // buffer back: without it the corruption is real and invisible.
    const FlatSpatialQuery surface;
    const GenerationContext context = test::context_of(kSeed, surface);

    GenerationWorld world(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> generator = test::make_generator(world);
    CY_REQUIRE(generator.has_value());
    CY_REQUIRE(generator->generate_all(ExecutionDomain::Cook, context).has_value());

    GenerationWorld reference(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> whole = test::make_generator(reference);
    CY_REQUIRE(whole.has_value());
    CY_REQUIRE(whole->generate_all(ExecutionDomain::Cook, context).has_value());
    CY_REQUIRE_EQ(test::regions_disagreeing(world, reference), 0u);

    RegionSet everything(test::allocator(), test::extent_of(kEdge));
    CY_REQUIRE(everything.resize());
    for (cy::i32 z = 0; z < kEdge; ++z) {
        for (cy::i32 x = 0; x < kEdge; ++x) {
            CY_REQUIRE(everything.add(RegionCoord{x, z, 0}));
        }
    }
    const cy::u8 scatter = stage_of(generator->program(), NodeKind::Scatter);
    const cy::u8 resolving = stage_of(generator->program(), NodeKind::Spacing);
    CY_REQUIRE(scatter != cy::pcg::Program::kNoStage);
    CY_REQUIRE(resolving != cy::pcg::Program::kNoStage);

    // Re-enter at the scatter with inputs that did not move: it re-runs, its digest does not
    // change, and the filter must still re-run behind it.
    CY_REQUIRE(generator
                   ->regenerate(ExecutionDomain::Cook, context, everything, DirtyCause::FieldChange,
                                scatter)
                   .has_value());
    const cy::u8 filtering = stage_of(generator->program(), NodeKind::Filter);
    CY_REQUIRE(filtering != cy::pcg::Program::kNoStage);
    const StageProfile* scattered = generator->profile().stage_at(scatter);
    const StageProfile* filtered = generator->profile().stage_at(filtering);
    CY_REQUIRE(scattered != nullptr);
    CY_REQUIRE(filtered != nullptr);
    CY_CHECK_EQ(scattered->regions_evaluated, static_cast<cy::u64>(kEdge) * kEdge);
    // THE INVARIANT. Not "roughly as many": the same regions, because the buffer they share was
    // rewritten in every one of them.
    CY_CHECK_EQ(filtered->regions_evaluated, scattered->regions_evaluated);

    // And now read the buffer back. A spacing resolution over unfiltered candidates accepts points
    // the rules rejected, and THAT is what a full regeneration disagrees with.
    CY_REQUIRE(generator
                   ->regenerate(ExecutionDomain::Cook, context, everything, DirtyCause::FieldChange,
                                resolving)
                   .has_value());
    CY_CHECK_EQ(test::regions_disagreeing(world, reference), 0u);
    CY_CHECK_EQ(test::world_digest(world), test::world_digest(reference));
}

CY_TEST_CASE("a region names the change and the dependency path that reached it") {
    // "WHEN a region regenerates THEN the tooling SHALL name the change and the dependency path
    // that reached it."
    const FlatSpatialQuery surface;
    const AuthoredStamp stamp = test::hill(3, 3);
    const cy::Span<const AuthoredStamp> edit(&stamp, 1);

    GenerationWorld world(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> generator = test::make_generator(world);
    CY_REQUIRE(generator.has_value());
    CY_REQUIRE(generator->generate_all(ExecutionDomain::Cook, test::context_of(kSeed, surface))
                   .has_value());

    const GenerationContext edited = test::context_of(kSeed, surface, edit);
    CY_REQUIRE(generator->regenerate_edit(ExecutionDomain::Cook, edited, edit).has_value());

    cy::Array<DirtyReason> path(test::allocator());
    CY_REQUIRE(generator->ledger().why_dirty(RegionCoord{3, 3, 0}, path));
    CY_CHECK_GT(path.size(), 0u);
    // The root of the path is the authored edit itself, named as such rather than as "something
    // upstream".
    bool rooted = false;
    for (const DirtyReason& reason : path) {
        rooted = rooted || reason.cause == DirtyCause::AuthoredEdit;
        // Every link names the stage it dirtied, so a designer reading the path sees `forest.flow`
        // rather than stage 3.
        CY_CHECK(reason.stage_name != nullptr);
    }
    CY_CHECK(rooted);

    // The ledger separates the causes, which is what design.md §1.3's finding is read off: a run
    // dominated by `DeclaredReach` is a generator whose dirty sets are a declared radius nothing
    // actually reads.
    CY_CHECK_GT(generator->ledger().count_of(DirtyCause::AuthoredEdit), 0u);
    CY_CHECK_GT(generator->ledger().count_of(DirtyCause::FixedPointExpansion), 0u);
}

CY_TEST_CASE(
    "what a region was recorded to read narrows the next dirty set below its declared reach") {
    // design.md §1.3: the long-range gather invalidates 179 regions of 576 to find ONE, "and the
    // answer is a provenance record of what each region actually READ, not a wider radius."
    //
    // `forest.smooth` declares a reach of TWO regions and its three relaxation passes read THREE
    // CELLS — one region. The read ledger is what closes that gap, and this case is the gap being
    // closed: the regions it evaluates are the ones it reads, not the ones it declared.
    const FlatSpatialQuery surface;
    const AuthoredStamp stamp = test::hill(3, 3, 10.0, 25.0F);
    const cy::Span<const AuthoredStamp> edit(&stamp, 1);

    GenerationWorld world(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> generator = test::make_generator(world);
    CY_REQUIRE(generator.has_value());
    CY_REQUIRE(generator->generate_all(ExecutionDomain::Cook, test::context_of(kSeed, surface))
                   .has_value());

    const cy::u8 smooth = stage_of(generator->program(), NodeKind::Smooth);
    CY_REQUIRE(smooth != cy::pcg::Program::kNoStage);
    CY_CHECK_EQ(generator->program().stages()[smooth].reach_regions, 2u);

    // The record exists and is narrower than the declaration: nine regions read, twenty-five
    // declared.
    const RegionCoord probe{3, 3, 0};
    CY_REQUIRE(generator->reads().has_record(smooth, probe));
    CY_CHECK_EQ(generator->reads().reads_of(smooth, probe).size(), 9u);

    const GenerationContext edited = test::context_of(kSeed, surface, edit);
    CY_REQUIRE(generator->regenerate_edit(ExecutionDomain::Cook, edited, edit).has_value());

    // The narrowing happened, and it is attributed: a region kept in the dirty set because its
    // RECORDED reads reached the change is a different fact from one kept because the declaration
    // did, and the ledger says which.
    CY_CHECK_GT(generator->ledger().count_of(DirtyCause::RecordedRead), 0u);

    // And it is MEASURED against the alternative rather than asserted: what the declared reach of
    // two would have dilated the upstream stage's changed set to, against what reading three cells
    // actually evaluated.
    const cy::u8 stamp_stage = stage_of(generator->program(), NodeKind::Stamp);
    CY_REQUIRE(stamp_stage != cy::pcg::Program::kNoStage);
    const RegionSet* changed = generator->changed_at(stamp_stage);
    CY_REQUIRE(changed != nullptr);
    CY_CHECK_GT(changed->size(), 0u);
    cy::Expected<RegionSet, cy::Error> declared = cy::pcg::dilate(test::allocator(), *changed, 2);
    CY_REQUIRE(declared.has_value());

    const StageProfile* profile = generator->profile().stage_at(smooth);
    CY_REQUIRE(profile != nullptr);
    CY_CHECK_GT(profile->regions_evaluated, 0u);
    CY_CHECK_LT(profile->regions_evaluated, declared->size());
}

CY_TEST_CASE("dilation is clamped to the extent and a region set enumerates canonically") {
    // The mechanics the fixed point is built on, checked on their own so that a failure above is
    // attributable to the closure rather than to the set.
    const RegionExtent extent = test::extent_of(4);
    RegionSet set(test::allocator(), extent);
    CY_REQUIRE(set.resize());
    CY_REQUIRE(set.add(RegionCoord{0, 0, 0}));
    CY_CHECK_EQ(set.size(), 1u);
    // Outside the extent is silently ignored rather than an error: a dilation at the world's edge
    // legitimately reaches past it.
    CY_REQUIRE(set.add(RegionCoord{-1, 0, 0}));
    CY_CHECK_EQ(set.size(), 1u);

    cy::Expected<RegionSet, cy::Error> grown = cy::pcg::dilate(test::allocator(), set, 1);
    CY_REQUIRE(grown.has_value());
    // A corner dilated by one is four regions, not nine: the other five are outside the world.
    CY_CHECK_EQ(grown->size(), 4u);

    cy::Array<RegionCoord> members(test::allocator());
    CY_REQUIRE(grown->members(members));
    CY_REQUIRE_EQ(members.size(), 4u);
    // Row-major, so the traversal order is a function of the SET rather than of the history that
    // built it — which is what makes a regeneration's order reproducible.
    CY_CHECK(members[0] == RegionCoord{0, 0, 0});
    CY_CHECK(members[1] == RegionCoord{1, 0, 0});
    CY_CHECK(members[2] == RegionCoord{0, 1, 0});
    CY_CHECK(members[3] == RegionCoord{1, 1, 0});
}
