#pragma once
// The evaluator: region state, the stage pipeline, the fixed point, and the budgeted runtime step.
// M10 tasks 4.1 and 4.3.
//
// `procedural-content-generation` — "Generation regions": "Generation SHALL be partitioned into
// regions, and world-scale generators SHALL NOT execute as one operation"; "Execution domains": "A
// generator not declared for a domain SHALL NOT run in it"; "Runtime generation": "incremental and
// budgeted ... cancellable"; "Spatial queries": "Build-time generation SHALL NOT require a running
// physics world in order to perform geometric queries; canonical geometric representations SHALL be
// used. Queries SHALL be batchable, since generation issues them in bulk."
//
// ================================================================================================
// WHAT RUNS, AND IN WHAT ORDER
// ================================================================================================
//
// A compiled program is a flat sequence of stages in topological order. The evaluator walks it
// STAGE BY STAGE ACROSS REGIONS, not region by region across stages, and the order is the whole
// reason a partial regeneration reproduces a full one:
//
//   * a gather stage reads its neighbours' output OF THE PREVIOUS STAGE, which is complete for
//     every region before the stage begins — in a full run and in a partial one alike;
//   * the conflict-resolution stage reads its neighbours' CANDIDATES, which are the output of the
//     previous stage by the same argument, and never their accepted points, which would be the
//     output of the stage currently running. `program.h` refuses a node that asks for the latter.
//
// Region-major evaluation would have made both of those false, and would have produced exactly the
// spike's `ordered` configuration: 2 of 12 trials reproduced, at best.
//
// ================================================================================================
// AND THE FIXED POINT, WHICH IS THE ONLY CLOSURE THIS MODULE OFFERS
// ================================================================================================
//
// After each stage, the regions whose output DIGEST changed seed the next stage's dirty set,
// dilated by that stage's declared reach or — where one exists — by what the region was recorded to
// have read. An ITERATIVE stage additionally expands its own active set while any region's output
// keeps changing, which is the mechanism that follows a transitive edge across a watershed. See
// `invalidation.h` for the 12-of-12 against 7-of-12 the spike measured.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/pcg/adapters.h>
#include <cy/pcg/dataset.h>
#include <cy/pcg/diagnostics.h>
#include <cy/pcg/identity.h>
#include <cy/pcg/invalidation.h>
#include <cy/pcg/program.h>

namespace cy::environment {
class FieldStore;
}  // namespace cy::environment

namespace cy::pcg {

/// The four sides of a region, in the order the boundary arrays use: north (-z), east, south (+z),
/// west.
inline constexpr u32 kRegionSides = 4;
inline constexpr i32 kSideDx[kRegionSides] = {0, 1, 0, -1};
inline constexpr i32 kSideDz[kRegionSides] = {-1, 0, 1, 0};

/// An authored edit: a radial stamp on a raster channel. The seed of every dirty set.
struct AuthoredStamp {
    /// Absolute world metres, so a stamp is placed where the author placed it rather than in some
    /// region's local frame.
    f64 x = 0.0;
    f64 z = 0.0;
    f64 radius = 0.0;
    f32 amount = 0.0F;
    /// The `Stamp` stage this belongs to, by node identity. A stamp set is shared by the whole
    /// program and each stamp names its own node, so two stamp nodes do not have to be handed two
    /// arrays that must be kept in step.
    NodeIdentity node;
};

/// The common spatial query service. `procedural-content-generation` — "Spatial queries".
///
/// An interface rather than a dependency, and that is the requirement rather than a style
/// preference: "Build-time generation SHALL NOT require a running physics world in order to perform
/// geometric queries." A cook binds a terrain representation, an editor binds the live one, and a
/// unit test binds an analytic surface — and `cy::pcg` links none of them. The bridge to
/// `cy::terrain` lives in the `cy::pcg-adapters` target for the same reason.
///
/// BATCHED, because "queries SHALL be batchable, since generation issues them in bulk": the
/// evaluator calls each of these once per region with `kRegionCellCount` positions, never once per
/// point.
class SpatialQuery {
public:
    SpatialQuery() = default;
    SpatialQuery(const SpatialQuery&) = delete;
    SpatialQuery& operator=(const SpatialQuery&) = delete;
    virtual ~SpatialQuery() = default;

    /// Ground height in metres at each (x, z). `out` has the same length as `x` and `z`.
    virtual void height_batch(Span<const f64> x, Span<const f64> z,
                              Span<f32> out) const noexcept = 0;
    /// Surface slope in radians at each position.
    virtual void slope_batch(Span<const f64> x, Span<const f64> z,
                             Span<f32> out) const noexcept = 0;
    /// Whether the surface exists at all: a hole, a cave mouth, a cliff face with no ground under
    /// it. False means "there is no surface here", which is not the same as a height of zero.
    virtual void surface_batch(Span<const f64> x, Span<const f64> z,
                               Span<u8> out) const noexcept = 0;
};

/// The query a generator gets when nothing bound one: a flat world at y = 0 with no slope.
///
/// Named rather than a null pointer, so a generator running without a terrain representation
/// produces a defined result and says which one it was, instead of branching on a pointer in three
/// places.
class FlatSpatialQuery final : public SpatialQuery {
public:
    explicit FlatSpatialQuery(f32 height = 0.0F) noexcept : height_(height) {}

    void height_batch(Span<const f64> x, Span<const f64> z, Span<f32> out) const noexcept override;
    void slope_batch(Span<const f64> x, Span<const f64> z, Span<f32> out) const noexcept override;
    void surface_batch(Span<const f64> x, Span<const f64> z, Span<u8> out) const noexcept override;

private:
    f32 height_ = 0.0F;
};

/// Everything one region holds after a generation.
///
/// A partial regeneration overwrites some of these for some regions and leaves the rest exactly as
/// the previous run left them, which is what makes the comparison against a full regeneration mean
/// anything at all.
struct RegionState {
    explicit RegionState(Allocator& allocator) noexcept
        : raster(allocator),
          candidates(allocator),
          accepted(allocator),
          provenance(allocator),
          stage_digests(allocator) {}

    RegionState(const RegionState&) = delete;
    RegionState& operator=(const RegionState&) = delete;
    RegionState(RegionState&&) noexcept = default;
    RegionState& operator=(RegionState&&) noexcept = default;

    RegionCoord coord;
    Raster raster;
    /// Candidates BEFORE conflict resolution. Retained because the conflict-resolution stage reads
    /// its neighbours' candidates, and a clean neighbour must supply them from here rather than be
    /// recomputed — which is also why a candidate list must be a pure function of that neighbour's
    /// own inputs.
    PointSet candidates;
    /// The accepted result.
    PointSet accepted;
    RegionProvenance provenance;

    /// Water leaving through each boundary cell, for `NodeKind::Propagate`. Four sides of
    /// `kRegionCells`.
    f32 outflow[kRegionSides * kRegionCells] = {};

    /// One digest per stage, from the last evaluation. The comparison the fixed point and the cache
    /// are both built on.
    Array<u64> stage_digests;
    /// Which stages have ever been evaluated for this region. A partial regeneration over a region
    /// with holes in this is refused, by name, rather than producing a plausible wrong answer.
    u64 evaluated_mask = 0;

    /// The run this region was last touched in. See `Generator::run_id_`.
    u64 touched_run = 0;

    /// Macro state: the region's coarse summary, maintained whether or not detail exists.
    /// `procedural-content-generation` — "Macro state and materialisation". Averages of the raster
    /// channels the program declares, which is why a materialised region is consistent with its
    /// macro state by construction rather than by a reconciliation pass.
    f32 macro_density = 0.0F;
    f32 macro_height = 0.0F;
    u32 macro_instances = 0;

    [[nodiscard]] bool has_stage(u8 stage) const noexcept {
        return stage < 64 && (evaluated_mask & (1ULL << stage)) != 0;
    }
    [[nodiscard]] u64 bytes() const noexcept;
};

/// The generated world: one `RegionState` per region that has ever been generated.
class GenerationWorld {
public:
    GenerationWorld(Allocator& allocator, const RegionExtent& extent) noexcept
        : allocator_(&allocator), states_(allocator), index_(allocator), extent_(extent) {}

    GenerationWorld(const GenerationWorld&) = delete;
    GenerationWorld& operator=(const GenerationWorld&) = delete;

    [[nodiscard]] const RegionExtent& extent() const noexcept { return extent_; }
    [[nodiscard]] Allocator& allocator() const noexcept { return *allocator_; }

    [[nodiscard]] RegionState* find(const RegionCoord& region) noexcept;
    [[nodiscard]] const RegionState* find(const RegionCoord& region) const noexcept;
    [[nodiscard]] Expected<RegionState*, Error> ensure(const RegionCoord& region) noexcept;

    /// Drop a region's detail. Its macro state is kept — that is the demotion the specification
    /// requires: "Demotion SHALL update macro state from the detailed representation, so the round
    /// trip does not lose what happened."
    [[nodiscard]] Status demote(const RegionCoord& region) noexcept;

    [[nodiscard]] usize resident() const noexcept { return states_.size(); }
    [[nodiscard]] u64 bytes() const noexcept;

    /// Whether every region of the extent has been generated through `stage`. A partial
    /// regeneration over an incomplete world is refused, because a gather would read a region that
    /// has no values and would produce a result no full regeneration could reproduce.
    [[nodiscard]] bool complete_through(u8 stage) const noexcept;

private:
    Allocator* allocator_;
    Array<RegionState> states_;
    HashMap<u64, usize> index_;
    RegionExtent extent_;
};

/// What one regeneration is given.
struct GenerationContext {
    /// The world seed. With the program digest and the region, the whole of the derivation.
    u64 seed = 0;
    /// Where fields are read from. Null is legitimate: a generator with no `FieldRead` stage never
    /// touches it, and one with a `FieldRead` stage and no store reads the field's declared
    /// default, which is what the substrate returns for an absent region anyway.
    const environment::FieldStore* fields = nullptr;
    /// The geometric representation. Never null in practice — bind `FlatSpatialQuery` rather than
    /// leaving it — and the evaluator substitutes one when it is.
    const SpatialQuery* geometry = nullptr;
    /// Absolute world metres of the region grid's origin: region (0, 0) starts here.
    f64 origin_x = 0.0;
    f64 origin_z = 0.0;
    /// Authored edits.
    Span<const AuthoredStamp> stamps;
    ProvenanceMode provenance = ProvenanceMode::On;
    /// Where the `Output` stage's result goes. Null means the run computes and keeps the result
    /// without building a representation, which is what the editor's region debugger and every
    /// suite in this module want.
    const OutputRegistry* outputs = nullptr;
    OutputTarget output_target = OutputTarget::Foliage;
};

/// How a budgeted run ended. `procedural-content-generation` — "Runtime generation": "A runtime
/// generator SHALL NOT block simulation or presentation."
enum class RunOutcome : u8 {
    /// Every dirty region was evaluated through every stage.
    Complete = 0,
    /// The step budget ran out. The run's state is intact; call `step()` again.
    BudgetExhausted,
    /// `cancel()` was called. The dirty set is retained, so resuming is the same call.
    Cancelled,
};

[[nodiscard]] const char* run_outcome_name(RunOutcome outcome) noexcept;

/// What a step did.
struct RunProgress {
    RunOutcome outcome = RunOutcome::Complete;
    u32 regions_evaluated = 0;
    u8 stage = 0;
    /// True when an iterative stage hit its declared bound without converging. NOT a budget: a
    /// `Convergence` policy's bound is a refusal point, so this is a defect in the generator rather
    /// than a result, and `Generator::run()` returns an error when it happens.
    bool iteration_exhausted = false;
};

/// A compiled generator, bound to a world.
///
/// One `Generator` per (program, world). It owns the dirty sets and the cursor a budgeted run
/// resumes from, which is why a run is a method here rather than a free function over state the
/// caller assembles.
class Generator {
public:
    [[nodiscard]] static Expected<Generator, Error> create(Allocator& allocator, Program program,
                                                           GenerationWorld& world) noexcept;

    Generator(const Generator&) = delete;
    Generator& operator=(const Generator&) = delete;
    Generator(Generator&&) noexcept = default;
    Generator& operator=(Generator&&) noexcept = default;

    [[nodiscard]] const Program& program() const noexcept { return program_; }
    [[nodiscard]] GenerationProfile& profile() noexcept { return profile_; }
    [[nodiscard]] const GenerationProfile& profile() const noexcept { return profile_; }
    [[nodiscard]] InvalidationLedger& ledger() noexcept { return ledger_; }
    [[nodiscard]] const InvalidationLedger& ledger() const noexcept { return ledger_; }
    [[nodiscard]] ReadLedger& reads() noexcept { return reads_; }

    /// Generate every region of the extent. The full regeneration a partial one is compared
    /// against.
    [[nodiscard]] Expected<RunProgress, Error> generate_all(
        ExecutionDomain domain, const GenerationContext& context) noexcept;

    /// Regenerate from a set of seed regions, entering the pipeline at `first_stage`.
    ///
    /// THE STAGE MATTERS AS MUCH AS THE REGIONS. An edit does not change what a `Noise` node
    /// computes, so seeding the dirty set at stage zero would evaluate the noise, find its digest
    /// unchanged, propagate nothing, and never reach the stage the edit actually feeds. The seed
    /// belongs at the stage that CONSUMES the change — which for an authored stamp is the stamp's
    /// own node, and `regenerate_edit()` below works that out from the stamps themselves.
    ///
    /// Refuses over a world that has never been fully generated, naming the reason.
    [[nodiscard]] Expected<RunProgress, Error> regenerate(ExecutionDomain domain,
                                                          const GenerationContext& context,
                                                          const RegionSet& seeds, DirtyCause cause,
                                                          u8 first_stage = 0) noexcept;

    /// Regenerate from a set of AUTHORED EDITS: the ordinary path, and the one that gets the stage
    /// right without the caller having to know the compiled program's shape.
    ///
    /// `changed` is the stamps that MOVED, not the whole stamp set — `context.stamps` is still the
    /// whole set, because the regions being re-evaluated need every stamp that reaches them. Each
    /// changed stamp seeds the dirty set of its own node's stage, so a program with a terrain stamp
    /// early and a road stamp late re-enters the pipeline twice rather than once at the earliest.
    [[nodiscard]] Expected<RunProgress, Error> regenerate_edit(
        ExecutionDomain domain, const GenerationContext& context,
        Span<const AuthoredStamp> changed) noexcept;

    /// The stage a node's identity compiled to, or `Program::kNoStage` when the compiler eliminated
    /// it or the graph never had it.
    [[nodiscard]] u8 stage_of(NodeIdentity node) const noexcept;

    /// The regions an edit touches, before any dilation: the seed of a dirty set.
    [[nodiscard]] Expected<RegionSet, Error> regions_touched(
        const GenerationContext& context, Span<const AuthoredStamp> stamps) const noexcept;

    /// Begin a budgeted, resumable run. `step()` carries it forward; `cancel()` abandons it at the
    /// next region boundary with the dirty set intact.
    [[nodiscard]] Status begin(ExecutionDomain domain, const GenerationContext& context,
                               const RegionSet& seeds, DirtyCause cause,
                               u8 first_stage = 0) noexcept;
    /// The edit-shaped `begin()`. See `regenerate_edit()`.
    [[nodiscard]] Status begin_edit(ExecutionDomain domain, const GenerationContext& context,
                                    Span<const AuthoredStamp> changed) noexcept;
    [[nodiscard]] Expected<RunProgress, Error> step(u32 max_regions) noexcept;
    void cancel() noexcept { cancelled_ = true; }
    /// Pick the run back up where `cancel()` put it down. The dirty set, the stage and the cursor
    /// were never thrown away, so this is a flag and not a re-plan — which is what makes cancelling
    /// a runtime generation cheap enough to do on any frame that runs short.
    void resume() noexcept { cancelled_ = false; }
    [[nodiscard]] bool running() const noexcept { return running_; }

    /// The regions whose output actually changed in the last run, per stage. What the fixed point
    /// propagates, and what a test compares against a full regeneration.
    [[nodiscard]] const RegionSet* changed_at(u8 stage) const noexcept;

    /// The region containing an absolute world position under this program's region size.
    [[nodiscard]] RegionCoord region_of(const GenerationContext& context, f64 x,
                                        f64 z) const noexcept;
    /// The absolute world metres of a region's minimum corner.
    void region_origin(const GenerationContext& context, const RegionCoord& region, f64& x,
                       f64& z) const noexcept;

private:
    Generator(Allocator& allocator, Program program, GenerationWorld& world) noexcept;

    /// The prologue every entry point shares: the domain refusal, the context, and the ledgers and
    /// sets reset. Split out so that the three ways to start a run cannot drift apart.
    [[nodiscard]] Status prepare(ExecutionDomain domain, const GenerationContext& context) noexcept;
    [[nodiscard]] Status seed_dirty(const RegionSet& seeds, DirtyCause cause, u8 stage) noexcept;
    [[nodiscard]] Status start_at(u8 stage) noexcept;
    [[nodiscard]] Status refuse_incomplete_world() const noexcept;
    [[nodiscard]] Expected<RunProgress, Error> drive(u32 max_regions) noexcept;
    /// End of one pass over an iterative stage's active set. True when another sweep is due —
    /// which, under `Convergence`, is while anything is still changing OR the fixed point is still
    /// growing the active set.
    [[nodiscard]] Expected<bool, Error> finish_pass() noexcept;
    [[nodiscard]] Status advance_stage() noexcept;
    [[nodiscard]] Expected<bool, Error> evaluate_one(u8 stage, const RegionCoord& region) noexcept;
    [[nodiscard]] Status record_change(u8 stage, const RegionCoord& region) noexcept;
    [[nodiscard]] Status expand_after_sweep(u8 stage) noexcept;
    [[nodiscard]] Status carry_to_next_stage(u8 stage) noexcept;

    Allocator* allocator_;
    Program program_;
    GenerationWorld* world_;
    GenerationContext context_;
    ExecutionDomain domain_ = ExecutionDomain::Editor;

    /// Per stage: the regions to evaluate, and the regions whose output changed.
    Array<RegionSet> dirty_;
    Array<RegionSet> changed_;
    /// The regions whose output changed in the CURRENT sweep of an iterative stage. Distinct from
    /// `changed_`, which accumulates across sweeps: the fixed point expands from what moved just
    /// now, and expanding from everything that has ever moved would re-dirty the world every sweep.
    Array<RegionSet> sweep_changed_set_;
    /// Per-region scratch the evaluator reuses, so a region's evaluation allocates nothing after
    /// the first. Batched spatial queries fill these.
    Array<f64> query_x_;
    Array<f64> query_z_;
    Array<f32> query_f_;
    Array<f32> query_g_;
    Array<u8> query_flags_;
    Array<u8> keep_;
    /// The canonical member order of the stage currently running, and how far through it the
    /// budgeted step has got.
    Array<RegionCoord> cursor_members_;
    usize cursor_ = 0;

    InvalidationLedger ledger_;
    ReadLedger reads_;
    GenerationProfile profile_;

    /// Which run this is. A region carries the run it was last touched in, so its provenance is
    /// cleared exactly once per run however many stages visit it — a clear keyed on "stage zero"
    /// would leave a partially-regenerated region accumulating a second copy of its rejections.
    u64 run_id_ = 0;
    u8 stage_ = 0;
    u32 sweep_ = 0;
    bool sweep_changed_ = false;
    bool running_ = false;
    bool cancelled_ = false;
    bool iteration_exhausted_ = false;
};

}  // namespace cy::pcg
