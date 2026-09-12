#pragma once
// The typed intermediate representation, the compiler, and the compiled program. M10 task 4.1.
//
// `procedural-content-generation` — "Graphs compile to programs": "The compiler SHALL apply at
// minimum: constant folding, dead-node elimination, filter fusion, spatial query fusion, attribute
// projection, parallelisation analysis, and classification of nodes eligible for GPU execution",
// and "WHEN a graph computes an attribute nothing consumes THEN the compiler SHALL eliminate it AND
// BE ABLE TO REPORT THAT IT DID" — which is why `CompileReport` carries the eliminated nodes by
// name rather than only a count.
//
// ================================================================================================
// THE COMPILER IS WHERE THE SPIKE'S CONDITIONS ARE ENFORCED
// ================================================================================================
//
// design.md §1.6 makes them requirements on this row rather than implementation details inside it,
// and a refusal at compile time is the shape M10 uses everywhere else: `environment-fields` refuses
// a second producer at registration, M8.c refused at cook time, M9 refused at configuration time.
// A diagnostic that fires at runtime has already shipped the defect.
//
//   `ReadsSiblingOutput`      a node declaring `NeighbourAccess::AcceptedOutput`. Order-dependent:
//                             the spike's `ordered` resolution reproduced a full regeneration in 2
//                             of 12 trials at best.
//   `IdentityFromTraversal`   a node declaring `TraversalCounter` or `SurvivorRank`. 43% and 3.7%
//                             of the spike's overrides silently MIS-BOUND, respectively.
//   `IterationWithoutBound`   an iterative node with no bound. "Generation cannot fail to
//                             terminate" is the requirement, and this is it checked.
//   `UnbudgetedRuntimeDomain` a `Runtime` or `Streaming` generator with no declared budget.
//
// A `Budget` iteration policy is NOT refused — it is a legitimate runtime lever — but it sets
// `Program::cacheable` false, and `cache.h` refuses to store an uncacheable region. The spike's
// `budget2` reproduced a full regeneration in 0 of 12 trials of all 12 of its configurations; the
// answer is not to forbid it but to stop recording it as if it were reproducible.
//
// ================================================================================================
// WHAT "COMPILED" MEANS HERE
// ================================================================================================
//
// A flat `Array<Stage>` in evaluation order, read by a `switch` in `execute.cpp`. No virtual node
// objects, no pointer chasing through an authored graph, and no `Graph` reachable from the
// evaluator: this header names the type by FORWARD DECLARATION only, so nothing that includes
// `program.h` can dereference an authored node. That is how "interpreted virtual-node graph
// traversal SHALL NOT appear in a hot execution path" becomes a fact about the include graph.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/pcg/dataset.h>
#include <cy/pcg/identity.h>
#include <cy/pcg/ops.h>

namespace cy::pcg {

/// The authoring layer, by forward declaration alone. `compile()` is the only function in this
/// module that takes one, and `graph.h` is included by `compile.cpp` and by callers who AUTHOR —
/// not by anything that executes. See ops.h.
class Graph;

/// Why a graph was refused, or what a pass did to it. A code rather than a string, so a test
/// asserts on the reason and a diagnostic prints both.
enum class CompileProblem : u8 {
    None = 0,
    /// A node names an input that is not in the graph.
    UnknownInput,
    /// Two nodes share an authored name. Identity derives from the name, so a duplicate would make
    /// two nodes mint the same identities.
    DuplicateName,
    /// The edges do not form a directed acyclic graph.
    Cycle,
    /// THE SPIKE'S FIRST CONDITION. A node declares `NeighbourAccess::AcceptedOutput`.
    ReadsSiblingOutput,
    /// THE SPIKE'S FOURTH CONDITION. A node mints identity from traversal order or from a rank.
    IdentityFromTraversal,
    /// An iterative node with no declared bound, or a cross-region solve with no declared policy.
    IterationWithoutBound,
    /// A relaxation declares more passes than its declared halo has cells, so it would read outside
    /// what it declared — and design.md §1.6's first condition says a node observed to read outside
    /// its declared reach is a defect the graph should NAME rather than tolerate. `kRegionCells`
    /// lives in `ops.h` so that this check is possible before anything runs.
    HaloTooSmallForIteration,
    /// A `Runtime` or `Streaming` generator with no declared budget.
    UnbudgetedRuntimeDomain,
    /// The graph declares no domain at all, so nothing could ever run it.
    NoDomainDeclared,
    /// The graph has no `Output` node.
    NoOutput,
    /// A node's input is the wrong shape — a point-set input where a raster is required, or the
    /// reverse. The typed half of the typed intermediate representation.
    TypeMismatch,
    /// A node writes an attribute that was never interned in the graph's own table.
    UnknownAttribute,
    /// Two raster nodes write the SAME channel.
    ///
    /// It is the one-producer-per-field rule, one level down and for the same reason: the
    /// invalidation gates a consumer on "did this stage's output change", and that question only
    /// has an answer while each stage owns what it writes. Two nodes on one channel would make the
    /// channel's value depend on which of them ran last, and the digest would say nothing changed.
    DuplicateChannel,
    /// A node declares a reach but no neighbour access, or an access but no reach.
    ReachWithoutAccess,
    /// More nodes than the program can address.
    TooManyNodes,
    kCount,
};

[[nodiscard]] const char* compile_problem_name(CompileProblem problem) noexcept;

/// One refusal, naming the node.
struct CompileDiagnostic {
    CompileProblem problem = CompileProblem::None;
    NodeId node;
    const char* node_name = "";
    /// The second party where a problem has one — the node whose name is duplicated, the node an
    /// edge points at. Empty otherwise.
    const char* other_name = "";
};

/// What the compiler decided about a node's parallelism. Reported rather than acted on, because the
/// task system's scheduling is `core-jobs-and-concurrency`'s and a second scheduler here would be
/// the parallel mechanism this project keeps refusing to build.
enum class Parallelism : u8 {
    /// Every region is independent: dispatch the whole dirty set at once.
    PerRegion = 0,
    /// Regions must be swept together until they converge — an iterative node.
    Swept,
    /// A barrier: a `Script` node, or anything downstream of one within its own stage.
    Barrier,
};

[[nodiscard]] const char* parallelism_name(Parallelism level) noexcept;

/// What the compiler decided about a node's GPU eligibility.
///
/// `procedural-content-generation` — "CPU and GPU execution": "The compiler SHALL classify nodes by
/// execution suitability", and "A generator declared deterministic SHALL only use GPU execution
/// where that execution meets its declared determinism level."
enum class GpuEligibility : u8 {
    /// Large-scale candidate generation, field sampling, noise, density evaluation and filtering.
    Eligible = 0,
    /// Constraint solving, adapter emission, anything with cross-region ordering.
    CpuOnly,
    /// Eligible by shape, but the generator is gameplay-deterministic and this host cannot promise
    /// the GPU reproduces the CPU. Recorded rather than scheduled — see the gap in the module
    /// README, and design.md §1.5's `pcg-gpu-domain-agreement`, which is NOT EVALUATED here.
    RefusedByDeterminism,
};

[[nodiscard]] const char* gpu_eligibility_name(GpuEligibility eligibility) noexcept;

/// What the compiler did. Every number here is measured during compilation, not estimated.
struct CompileReport {
    u32 nodes_in = 0;
    u32 nodes_out = 0;
    u32 folded = 0;
    u32 eliminated = 0;
    u32 filters_fused = 0;
    u32 queries_fused = 0;
    u32 attributes_projected = 0;
    u32 barriers = 0;
    u32 gpu_eligible = 0;
    u32 gpu_refused_by_determinism = 0;
    /// Nodes the author marked `gpu_hint` that the compiler classified `CpuOnly`. A disagreement is
    /// not an error and is worth printing: it is usually a node that grew a constraint solve.
    u32 gpu_hint_disagreements = 0;

    /// The names of the eliminated nodes, in graph order. The requirement asks the compiler to "be
    /// able to report that it did", and a count alone cannot answer "which attribute did I lose".
    Array<const char*> eliminated_names;
    /// The barrier nodes, so a slow generator's optimisation barriers are visible without a profile
    /// run. "WHEN a scripted node prevents fusion or parallelisation THEN the compiler SHALL report
    /// it."
    Array<const char*> barrier_names;

    explicit CompileReport(Allocator& allocator) noexcept
        : eliminated_names(allocator), barrier_names(allocator) {}

    CompileReport(const CompileReport&) = delete;
    CompileReport& operator=(const CompileReport&) = delete;
    CompileReport(CompileReport&&) noexcept = default;
    CompileReport& operator=(CompileReport&&) noexcept = default;
};

/// One instruction of the compiled program.
///
/// `inputs` are indices into the stage array — already topologically ordered, so a stage's inputs
/// are always stages with a lower index, and evaluation is a forward walk with no resolution step.
struct Stage {
    NodeKind kind = NodeKind::Constant;
    /// The node's identity, for derivation and diagnostics. Carried rather than looked up, because
    /// a lookup would put the authored graph back into the hot path.
    NodeIdentity identity;
    const char* name = "";

    u8 inputs[kMaxNodeInputs] = {0, 0, 0, 0};
    u8 input_count = 0;

    AttributeId output;
    AttributeId reads;
    /// A second point column a fused filter tests. `kMaxFusedFilters` of these live in `fused`.
    NeighbourAccess neighbour_access = NeighbourAccess::None;
    u8 reach_regions = 0;

    IterationPolicy iteration = IterationPolicy::None;
    u32 iteration_bound = 0;

    Parallelism parallelism = Parallelism::PerRegion;
    GpuEligibility gpu = GpuEligibility::CpuOnly;

    NodeParams params;

    /// Filters fused into this stage. A `Filter` whose only consumer is another `Filter` is folded
    /// into it, so a chain of four threshold tests is one pass over the point set rather than four.
    static constexpr u32 kMaxFusedFilters = 4;
    struct FusedFilter {
        AttributeId reads;
        f32 lower = 0.0F;
        f32 upper = 0.0F;
        bool range_test = false;
    };
    FusedFilter fused[kMaxFusedFilters];
    u8 fused_count = 0;

    /// True when this stage's raster read was fused into a neighbouring `FieldRead` — the spatial
    /// query fusion pass. Recorded so the profiler can attribute the saved sampling.
    bool query_fused = false;
};

/// A compiled generator. Immutable once produced.
class Program {
public:
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;
    Program(Program&&) noexcept = default;
    Program& operator=(Program&&) noexcept = default;

    [[nodiscard]] Span<const Stage> stages() const noexcept { return stages_.span(); }
    [[nodiscard]] const AttributeTable& attributes() const noexcept { return attributes_; }

    [[nodiscard]] const char* name() const noexcept { return name_; }
    [[nodiscard]] u32 version() const noexcept { return version_; }
    [[nodiscard]] DomainMask domains() const noexcept { return domains_; }
    [[nodiscard]] const GenerationBudget& budget() const noexcept { return budget_; }
    [[nodiscard]] DeterminismLevel determinism() const noexcept { return determinism_; }
    [[nodiscard]] f64 region_metres() const noexcept { return region_metres_; }
    [[nodiscard]] u8 region_level() const noexcept { return region_level_; }

    /// Whether a region's result may be stored in the derived data cache.
    ///
    /// False when any iterative stage runs to a BUDGET rather than to convergence. The spike:
    /// `budget2` reproduced a full regeneration in 0 of 12 trials, in every one of its 12
    /// configurations, because a partial run sweeps a different set of regions a different number
    /// of times and a budgeted result is a function of exactly that. `cache.h` refuses the store;
    /// this is where the refusal's reason is computed.
    [[nodiscard]] bool cacheable() const noexcept { return cacheable_; }

    /// The reach, in regions, of the whole program: the sum of every stage's declared reach. The
    /// worst case a dirty set can dilate to before the fixed point runs, and the number
    /// `invalidation.h` reports when it explains why a settle-shaped node is expensive.
    [[nodiscard]] u32 total_reach_regions() const noexcept { return total_reach_; }

    /// Every field this program reads, for dependency tracking. `procedural-content-generation` —
    /// "Field reads SHALL participate in dependency tracking."
    [[nodiscard]] Span<const u64> fields_read() const noexcept { return fields_.span(); }

    /// The digest of the whole compiled program: every stage, every parameter, every attribute
    /// name, the version and the region size. The first contribution to every derivation key.
    [[nodiscard]] u64 digest() const noexcept { return digest_; }

    /// The stage that mints identity, or `kNoStage`. There is at most one `Scatter` per program in
    /// this implementation — see the module README's gaps.
    static constexpr u8 kNoStage = 0xFF;
    [[nodiscard]] u8 scatter_stage() const noexcept { return scatter_stage_; }
    [[nodiscard]] u8 output_stage() const noexcept { return output_stage_; }
    /// The conflict-resolution stage, or `kNoStage`. Its presence is what decides whether a
    /// region's ACCEPTED set is the resolution's output or the candidate list itself.
    [[nodiscard]] u8 spacing_stage() const noexcept { return spacing_stage_; }

    /// The two columns the evaluator owns on every candidate set, interned by the compiler rather
    /// than by an author: a candidate's PRIORITY, which order-free conflict resolution compares,
    /// and the DENSITY the scatter tested. They are in the program's table so that a diagnostic can
    /// name them and so that nothing in the evaluator resolves a column by string.
    [[nodiscard]] AttributeId priority_attribute() const noexcept { return priority_; }
    [[nodiscard]] AttributeId density_attribute() const noexcept { return density_; }

    /// The raster channels a region's macro summary is taken from: the density the scatter reads
    /// and the first elevation-shaped channel the program computes. `procedural-content-generation`
    /// — "Macro state and materialisation": a region that is not resident is "a small number of
    /// field values", and these are them.
    [[nodiscard]] AttributeId macro_density_channel() const noexcept { return macro_density_; }
    [[nodiscard]] AttributeId macro_height_channel() const noexcept { return macro_height_; }

    /// Every raster channel the program writes, for the region raster's one allocation.
    [[nodiscard]] Span<const AttributeId> raster_channels() const noexcept {
        return channels_.span();
    }

    [[nodiscard]] bool may_run_in(ExecutionDomain domain) const noexcept {
        return domains_.has(domain);
    }

private:
    friend Expected<Program, Error> compile(Allocator&, const Graph&, CompileReport&,
                                            Array<CompileDiagnostic>&) noexcept;

    explicit Program(Allocator& allocator) noexcept
        : stages_(allocator), attributes_(allocator), fields_(allocator), channels_(allocator) {}

    Array<Stage> stages_;
    AttributeTable attributes_;
    Array<u64> fields_;
    Array<AttributeId> channels_;
    AttributeId priority_;
    AttributeId density_;
    AttributeId macro_density_;
    AttributeId macro_height_;

    const char* name_ = "";
    u32 version_ = 0;
    DomainMask domains_;
    GenerationBudget budget_;
    DeterminismLevel determinism_ = DeterminismLevel::Gameplay;
    f64 region_metres_ = 64.0;
    u8 region_level_ = 0;
    bool cacheable_ = true;
    u32 total_reach_ = 0;
    u64 digest_ = 0;
    u8 scatter_stage_ = kNoStage;
    u8 output_stage_ = kNoStage;
    u8 spacing_stage_ = kNoStage;
};

/// Compile a graph.
///
/// `diagnostics` is appended to, never cleared, so a caller compiling several generators collects
/// every refusal in one place. A non-empty `diagnostics` with a returned program is impossible: the
/// compiler refuses or succeeds.
[[nodiscard]] Expected<Program, Error> compile(Allocator& allocator, const Graph& graph,
                                               CompileReport& report,
                                               Array<CompileDiagnostic>& diagnostics) noexcept;

/// The compiler's own GPU classification, exposed so the suite can assert on it directly rather
/// than on a count in the report.
[[nodiscard]] GpuEligibility classify_gpu(NodeKind kind, bool gameplay_deterministic) noexcept;

/// The compiler's own parallelism classification, exposed for the same reason.
[[nodiscard]] Parallelism classify_parallelism(NodeKind kind, IterationPolicy iteration) noexcept;

}  // namespace cy::pcg
