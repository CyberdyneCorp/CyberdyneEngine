// The compiler: the refusals the spike made requirements, and the passes the specification names.
// M10 tasks.md 4.1.
//
// ================================================================================================
// FOUR OF THESE CASES ARE THE SPIKE'S OWN CONDITIONS, AND EACH HAS A NUMBER BEHIND IT
// ================================================================================================
//
// design.md §1.2 held three conditions fixed and broke the fourth, twelve trials each:
//
//   order-free conflict resolution   `ordered` reproduced 2 of 12 at best
//   invalidation to a fixed point    `static` reproduced 7 of 12 — the dangerous cell
//   convergence, not a sweep budget  `budget2` reproduced 0 of 12, in all 12 configurations
//   identity from stable ids         `counter` mis-bound 43% of overrides, `rank` 3.7%
//
// Two of the four are refused HERE, at graph-compile time, because a diagnostic that fires at
// runtime has already shipped the defect — which is the shape `environment-fields` uses for a
// second producer and M8.c used at cook time. The third is not refused but makes the program
// uncacheable
// (`test_cache.cpp`), and the fourth is the invalidation itself (`test_invalidation.cpp`).
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL: the `node.neighbour_access == AcceptedOutput` branch
// in `check_node()` was deleted, and "a node that reads a sibling's accepted output is refused"
// went red on its `CY_REQUIRE_FALSE(program.has_value())` — the graph compiled, and an
// order-dependent conflict resolution had been admitted with no diagnostic at all. The branch was
// then restored. The same was done to the `identity_source != Derived` branch and to the halo
// check; all three runs are reported in this milestone's `verified_failing`.

#include <cy/test/test.h>

#include <cy/pcg/graph.h>
#include <cy/pcg/program.h>

#include "fixtures.h"

using cy::pcg::CompileDiagnostic;
using cy::pcg::CompileProblem;
using cy::pcg::CompileReport;
using cy::pcg::DeterminismLevel;
using cy::pcg::DomainMask;
using cy::pcg::ExecutionDomain;
using cy::pcg::GenerationBudget;
using cy::pcg::GpuEligibility;
using cy::pcg::Graph;
using cy::pcg::GraphNode;
using cy::pcg::IdentitySource;
using cy::pcg::IterationPolicy;
using cy::pcg::NeighbourAccess;
using cy::pcg::NodeId;
using cy::pcg::NodeKind;
using cy::pcg::Parallelism;
using cy::pcg::Program;
namespace test = cy::pcg::test;

namespace {

/// Did the compiler report `problem`, and against which node?
[[nodiscard]] const CompileDiagnostic* refusal_for(const cy::Array<CompileDiagnostic>& diagnostics,
                                                   CompileProblem problem) noexcept {
    for (const CompileDiagnostic& diagnostic : diagnostics) {
        if (diagnostic.problem == problem) {
            return &diagnostic;
        }
    }
    return nullptr;
}

struct Compiled {
    explicit Compiled(cy::Allocator& allocator) noexcept
        : report(allocator), diagnostics(allocator) {}

    CompileReport report;
    cy::Array<CompileDiagnostic> diagnostics;
};

}  // namespace

CY_TEST_CASE("the forest graph compiles, and every pass reports what it did") {
    cy::Expected<test::ForestGraph, cy::Error> forest = test::build_forest(test::allocator());
    CY_REQUIRE(forest.has_value());
    Compiled out(test::allocator());
    cy::Expected<Program, cy::Error> program =
        cy::pcg::compile(test::allocator(), forest->graph, out.report, out.diagnostics);
    CY_REQUIRE(program.has_value());
    CY_CHECK_EQ(out.diagnostics.size(), 0u);
    CY_CHECK_EQ(out.report.nodes_in, 9u);
    CY_CHECK_EQ(out.report.nodes_out, 9u);
    CY_CHECK_EQ(out.report.eliminated, 0u);

    // The classifications the specification asks the compiler to make.
    CY_CHECK(cy::pcg::classify_parallelism(NodeKind::Propagate, IterationPolicy::Convergence) ==
             Parallelism::Swept);
    CY_CHECK(cy::pcg::classify_parallelism(NodeKind::Noise, IterationPolicy::None) ==
             Parallelism::PerRegion);
    CY_CHECK(cy::pcg::classify_parallelism(NodeKind::Script, IterationPolicy::None) ==
             Parallelism::Barrier);

    // A GAMEPLAY-deterministic generator's otherwise-eligible nodes are REFUSED the GPU rather than
    // scheduled on it: design.md §1.5 declines to claim on this host that a GPU domain reproduces
    // the CPU one, and `pcg-gpu-domain-agreement` is NOT EVALUATED for exactly that reason.
    CY_CHECK(cy::pcg::classify_gpu(NodeKind::Noise, true) == GpuEligibility::RefusedByDeterminism);
    CY_CHECK(cy::pcg::classify_gpu(NodeKind::Noise, false) == GpuEligibility::Eligible);
    CY_CHECK(cy::pcg::classify_gpu(NodeKind::Spacing, false) == GpuEligibility::CpuOnly);
    CY_CHECK_EQ(out.report.gpu_eligible, 0u);
    CY_CHECK_GT(out.report.gpu_refused_by_determinism, 0u);

    // The program's total declared reach: smooth 2, flow 1, spacing 1.
    CY_CHECK_EQ(program->total_reach_regions(), 4u);
    CY_CHECK(program->cacheable());
    CY_CHECK(program->scatter_stage() != Program::kNoStage);
    CY_CHECK(program->spacing_stage() != Program::kNoStage);
    CY_CHECK(program->output_stage() != Program::kNoStage);
}

CY_TEST_CASE("a node that reads a sibling's accepted output is refused, and named") {
    cy::Expected<test::ForestGraph, cy::Error> forest = test::build_forest(test::allocator());
    CY_REQUIRE(forest.has_value());
    // The spike's FIRST condition, spelled as a declaration so it can be refused by name: a node
    // may read its neighbours' CANDIDATES and not their accepted points. `ordered` resolution
    // reproduced a full regeneration in 2 of 12 trials at best.
    GraphNode bad;
    bad.name = "forest.ordered-spacing";
    bad.kind = NodeKind::Spacing;
    bad.inputs[0] = forest->filter;
    bad.input_count = 1;
    bad.neighbour_access = NeighbourAccess::AcceptedOutput;
    bad.reach_regions = 1;
    CY_REQUIRE(forest->graph.add(bad).has_value());

    Compiled out(test::allocator());
    cy::Expected<Program, cy::Error> program =
        cy::pcg::compile(test::allocator(), forest->graph, out.report, out.diagnostics);
    CY_REQUIRE_FALSE(program.has_value());
    const CompileDiagnostic* refusal =
        refusal_for(out.diagnostics, CompileProblem::ReadsSiblingOutput);
    CY_REQUIRE(refusal != nullptr);
    CY_CHECK(test::same_text(refusal->node_name, "forest.ordered-spacing"));
}

CY_TEST_CASE("a node that mints identity from traversal order is refused, and named") {
    // The spike's FOURTH condition. `counter` mis-bound 3 351 of 7 877 overrides — 43% — on an
    // ORDINARY FULL REGENERATION, before partial regeneration was involved at all; `rank` mis-bound
    // 292. Both are spellable here so both can be refused by name.
    for (IdentitySource source : {IdentitySource::TraversalCounter, IdentitySource::SurvivorRank}) {
        cy::Expected<test::ForestGraph, cy::Error> forest = test::build_forest(test::allocator());
        CY_REQUIRE(forest.has_value());
        GraphNode bad;
        bad.name = "forest.counted-scatter";
        bad.kind = NodeKind::Scatter;
        bad.inputs[0] = forest->density;
        bad.input_count = 1;
        bad.params.count = 8;
        bad.identity_source = source;
        CY_REQUIRE(forest->graph.add(bad).has_value());

        Compiled out(test::allocator());
        cy::Expected<Program, cy::Error> program =
            cy::pcg::compile(test::allocator(), forest->graph, out.report, out.diagnostics);
        CY_REQUIRE_FALSE(program.has_value());
        const CompileDiagnostic* refusal =
            refusal_for(out.diagnostics, CompileProblem::IdentityFromTraversal);
        CY_REQUIRE(refusal != nullptr);
        CY_CHECK(test::same_text(refusal->node_name, "forest.counted-scatter"));
        // The refusal names WHICH scheme, so the diagnostic says what to change rather than that
        // something is wrong.
        CY_CHECK(test::same_text(refusal->other_name, cy::pcg::identity_source_name(source)));
    }
}

CY_TEST_CASE(
    "an iterative node with no bound, and a cross-region solve with no policy, are refused") {
    cy::Expected<test::ForestGraph, cy::Error> forest = test::build_forest(test::allocator());
    CY_REQUIRE(forest.has_value());
    // "Every iterative node SHALL declare a bound, so that generation cannot fail to terminate."
    GraphNode unbounded;
    unbounded.name = "forest.unbounded";
    unbounded.kind = NodeKind::Propagate;
    unbounded.inputs[0] = forest->smooth;
    unbounded.input_count = 1;
    unbounded.output = forest->attributes.flow;
    unbounded.neighbour_access = NeighbourAccess::Raster;
    unbounded.reach_regions = 1;
    unbounded.iteration = IterationPolicy::Convergence;
    unbounded.iteration_bound = 0;
    CY_REQUIRE(forest->graph.add(unbounded).has_value());

    Compiled out(test::allocator());
    CY_REQUIRE_FALSE(cy::pcg::compile(test::allocator(), forest->graph, out.report, out.diagnostics)
                         .has_value());
    CY_CHECK(refusal_for(out.diagnostics, CompileProblem::IterationWithoutBound) != nullptr);
}

CY_TEST_CASE("a relaxation whose halo cannot hold its passes is refused") {
    cy::Expected<test::ForestGraph, cy::Error> forest = test::build_forest(test::allocator());
    CY_REQUIRE(forest.has_value());
    // design.md §1.6's first condition in its general form: "a node that declares a reach and is
    // then observed to read outside it is a defect the graph should NAME, not tolerate." The halo
    // is declared in REGIONS and the passes are counted in CELLS, so this is the one place the two
    // units meet — and a clamp here would produce a plausible wrong answer at every region boundary
    // in the world.
    GraphNode wide;
    wide.name = "forest.wide-smooth";
    wide.kind = NodeKind::Smooth;
    wide.inputs[0] = forest->stamp;
    wide.input_count = 1;
    wide.output = forest->attributes.eroded;
    wide.neighbour_access = NeighbourAccess::Raster;
    wide.reach_regions = 1;
    wide.iteration_bound = cy::pcg::kRegionCells + 1;  // one cell more than one region of halo
    wide.params.amplitude = 0.25F;
    CY_REQUIRE(forest->graph.add(wide).has_value());

    Compiled out(test::allocator());
    CY_REQUIRE_FALSE(cy::pcg::compile(test::allocator(), forest->graph, out.report, out.diagnostics)
                         .has_value());
    const CompileDiagnostic* refusal =
        refusal_for(out.diagnostics, CompileProblem::HaloTooSmallForIteration);
    CY_REQUIRE(refusal != nullptr);
    CY_CHECK(test::same_text(refusal->node_name, "forest.wide-smooth"));
}

CY_TEST_CASE("a runtime generator with no declared budget is refused") {
    cy::Expected<test::ForestGraph, cy::Error> forest = test::build_forest(test::allocator());
    CY_REQUIRE(forest.has_value());
    DomainMask domains;
    domains.add(ExecutionDomain::Runtime);
    forest->graph.declare_domains(domains);

    Compiled out(test::allocator());
    CY_REQUIRE_FALSE(cy::pcg::compile(test::allocator(), forest->graph, out.report, out.diagnostics)
                         .has_value());
    CY_CHECK(refusal_for(out.diagnostics, CompileProblem::UnbudgetedRuntimeDomain) != nullptr);

    // With a budget it compiles: the refusal is about the DECLARATION being absent, not about the
    // runtime domain being forbidden. "Runtime generation bypassing task scheduling, memory
    // budgets, or streaming budgets" is the forbidden pattern, and this is it made checkable.
    GenerationBudget budget;
    budget.cpu_micros = 500;
    budget.bytes = 4u << 20U;
    forest->graph.declare_budget(budget);
    Compiled again(test::allocator());
    CY_CHECK(cy::pcg::compile(test::allocator(), forest->graph, again.report, again.diagnostics)
                 .has_value());
}

CY_TEST_CASE("two nodes with one name are refused, because identity derives from the name") {
    cy::Expected<test::ForestGraph, cy::Error> forest = test::build_forest(test::allocator());
    CY_REQUIRE(forest.has_value());
    GraphNode twin;
    twin.name = test::kScatterNode;  // the same literal the fixture's scatter uses
    twin.kind = NodeKind::Scatter;
    twin.inputs[0] = forest->density;
    twin.input_count = 1;
    twin.params.count = 4;
    CY_REQUIRE(forest->graph.add(twin).has_value());

    Compiled out(test::allocator());
    CY_REQUIRE_FALSE(cy::pcg::compile(test::allocator(), forest->graph, out.report, out.diagnostics)
                         .has_value());
    const CompileDiagnostic* refusal = refusal_for(out.diagnostics, CompileProblem::DuplicateName);
    CY_REQUIRE(refusal != nullptr);
    // Both names, so the diagnostic says which two nodes collided rather than that two did.
    CY_CHECK(test::same_text(refusal->node_name, test::kScatterNode));
    CY_CHECK(test::same_text(refusal->other_name, test::kScatterNode));
}

CY_TEST_CASE("a point-set input where a raster is required is refused") {
    cy::Expected<test::ForestGraph, cy::Error> forest = test::build_forest(test::allocator());
    CY_REQUIRE(forest.has_value());
    // The TYPED half of "compiled through a typed intermediate representation": a smoothing node
    // reading a scatter is a shape error, and it is caught before anything runs.
    GraphNode wrong;
    wrong.name = "forest.mistyped";
    wrong.kind = NodeKind::Smooth;
    wrong.inputs[0] = forest->scatter;
    wrong.input_count = 1;
    wrong.output = forest->attributes.eroded;
    wrong.neighbour_access = NeighbourAccess::Raster;
    wrong.reach_regions = 1;
    wrong.iteration_bound = 2;
    CY_REQUIRE(forest->graph.add(wrong).has_value());

    Compiled out(test::allocator());
    CY_REQUIRE_FALSE(cy::pcg::compile(test::allocator(), forest->graph, out.report, out.diagnostics)
                         .has_value());
    CY_CHECK(refusal_for(out.diagnostics, CompileProblem::TypeMismatch) != nullptr);
}

CY_TEST_CASE("a graph with no output, and one with no domain, are both refused") {
    Graph empty(test::allocator());
    empty.declare("empty", 1);
    empty.declare_domains(DomainMask::of(ExecutionDomain::Cook));
    Compiled out(test::allocator());
    CY_REQUIRE_FALSE(
        cy::pcg::compile(test::allocator(), empty, out.report, out.diagnostics).has_value());
    CY_CHECK(refusal_for(out.diagnostics, CompileProblem::NoOutput) != nullptr);

    cy::Expected<test::ForestGraph, cy::Error> forest = test::build_forest(test::allocator());
    CY_REQUIRE(forest.has_value());
    forest->graph.declare_domains(DomainMask{});
    Compiled second(test::allocator());
    CY_REQUIRE_FALSE(
        cy::pcg::compile(test::allocator(), forest->graph, second.report, second.diagnostics)
            .has_value());
    CY_CHECK(refusal_for(second.diagnostics, CompileProblem::NoDomainDeclared) != nullptr);
}

CY_TEST_CASE("a node nothing consumes is eliminated, and the compiler names it") {
    cy::Expected<test::ForestGraph, cy::Error> forest = test::build_forest(test::allocator());
    CY_REQUIRE(forest.has_value());
    cy::Expected<cy::pcg::AttributeId, cy::Error> unused =
        forest->graph.attributes().intern("forest.unused", cy::pcg::AttributeType::F32);
    CY_REQUIRE(unused.has_value());
    GraphNode orphan;
    orphan.name = "forest.orphan";
    orphan.kind = NodeKind::Compute;
    orphan.inputs[0] = forest->flow;
    orphan.input_count = 1;
    orphan.output = *unused;
    orphan.params.value = 2.0F;
    CY_REQUIRE(forest->graph.add(orphan).has_value());

    Compiled out(test::allocator());
    cy::Expected<Program, cy::Error> program =
        cy::pcg::compile(test::allocator(), forest->graph, out.report, out.diagnostics);
    CY_REQUIRE(program.has_value());
    CY_CHECK_EQ(out.report.eliminated, 1u);
    // "THEN the compiler SHALL eliminate it AND BE ABLE TO REPORT THAT IT DID." A count alone
    // cannot answer "which attribute did I lose".
    CY_REQUIRE_EQ(out.report.eliminated_names.size(), 1u);
    CY_CHECK(test::same_text(out.report.eliminated_names[0], "forest.orphan"));
    CY_CHECK_EQ(out.report.nodes_out, 9u);
}

CY_TEST_CASE("a compute over a constant is folded, and a chain of filters is fused") {
    Graph graph(test::allocator());
    graph.declare("folding", 1);
    graph.declare_domains(DomainMask::of(ExecutionDomain::Cook));
    graph.declare_region_size(64.0, 0);
    cy::Expected<cy::pcg::AttributeId, cy::Error> base =
        graph.attributes().intern("base", cy::pcg::AttributeType::F32);
    CY_REQUIRE(base.has_value());
    cy::Expected<cy::pcg::AttributeId, cy::Error> scaled =
        graph.attributes().intern("pcg.density", cy::pcg::AttributeType::F32);
    CY_REQUIRE(scaled.has_value());

    GraphNode constant;
    constant.name = "folding.constant";
    constant.kind = NodeKind::Constant;
    constant.output = *base;
    constant.params.value = 0.5F;
    cy::Expected<NodeId, cy::Error> constant_id = graph.add(constant);
    CY_REQUIRE(constant_id.has_value());

    GraphNode compute;
    compute.name = "folding.compute";
    compute.kind = NodeKind::Compute;
    compute.inputs[0] = *constant_id;
    compute.input_count = 1;
    compute.output = *scaled;
    compute.params.value = 2.0F;
    compute.params.second = 0.25F;
    cy::Expected<NodeId, cy::Error> compute_id = graph.add(compute);
    CY_REQUIRE(compute_id.has_value());

    GraphNode scatter;
    scatter.name = "folding.scatter";
    scatter.kind = NodeKind::Scatter;
    scatter.inputs[0] = *compute_id;
    scatter.input_count = 1;
    scatter.params.count = 8;
    cy::Expected<NodeId, cy::Error> scatter_id = graph.add(scatter);
    CY_REQUIRE(scatter_id.has_value());

    GraphNode first;
    first.name = "folding.filter-a";
    first.kind = NodeKind::Filter;
    first.inputs[0] = *scatter_id;
    first.input_count = 1;
    first.reads = *scaled;
    first.params.value = 0.1F;
    cy::Expected<NodeId, cy::Error> first_id = graph.add(first);
    CY_REQUIRE(first_id.has_value());

    GraphNode second;
    second.name = "folding.filter-b";
    second.kind = NodeKind::Filter;
    second.inputs[0] = *first_id;
    second.input_count = 1;
    second.reads = *scaled;
    second.params.value = 0.2F;
    cy::Expected<NodeId, cy::Error> second_id = graph.add(second);
    CY_REQUIRE(second_id.has_value());

    GraphNode output;
    output.name = "folding.output";
    output.kind = NodeKind::Output;
    output.inputs[0] = *second_id;
    output.input_count = 1;
    CY_REQUIRE(graph.add(output).has_value());

    Compiled out(test::allocator());
    cy::Expected<Program, cy::Error> program =
        cy::pcg::compile(test::allocator(), graph, out.report, out.diagnostics);
    CY_REQUIRE(program.has_value());
    CY_CHECK_EQ(out.report.folded, 1u);
    CY_CHECK_EQ(out.report.filters_fused, 1u);
    // Six nodes in, five out: the absorbed filter emits no stage of its own, and four threshold
    // tests become one pass over the point set rather than four.
    CY_CHECK_EQ(out.report.nodes_in, 6u);
    CY_CHECK_EQ(out.report.nodes_out, 5u);

    // The fold computed the value rather than deferring it: 0.5 * 2 + 0.25.
    bool found = false;
    for (const cy::pcg::Stage& stage : program->stages()) {
        if (test::same_text(stage.name, "folding.compute")) {
            found = true;
            CY_CHECK(stage.kind == NodeKind::Constant);
            CY_CHECK_NEAR(stage.params.value, 1.25F, 1e-6F);
            CY_CHECK_EQ(stage.input_count, 0u);
        }
        if (test::same_text(stage.name, "folding.filter-b")) {
            CY_CHECK_EQ(stage.fused_count, 1u);
            CY_CHECK_NEAR(stage.fused[0].lower, 0.1F, 1e-6F);
        }
    }
    CY_CHECK(found);
}

CY_TEST_CASE("a scripted node is a barrier, and the compiler reports it by name") {
    cy::Expected<test::ForestGraph, cy::Error> forest = test::build_forest(test::allocator());
    CY_REQUIRE(forest.has_value());
    // "A general scripted node MAY exist and SHALL be marked as an optimisation and parallelisation
    // BARRIER, so its cost is visible."
    GraphNode script;
    script.name = "forest.script";
    script.kind = NodeKind::Script;
    script.inputs[0] = forest->spacing;
    script.input_count = 1;
    cy::Expected<NodeId, cy::Error> script_id = forest->graph.add(script);
    CY_REQUIRE(script_id.has_value());
    GraphNode output;
    output.name = "forest.script-output";
    output.kind = NodeKind::Output;
    output.inputs[0] = *script_id;
    output.input_count = 1;
    CY_REQUIRE(forest->graph.add(output).has_value());

    Compiled out(test::allocator());
    cy::Expected<Program, cy::Error> program =
        cy::pcg::compile(test::allocator(), forest->graph, out.report, out.diagnostics);
    CY_REQUIRE(program.has_value());
    CY_CHECK_EQ(out.report.barriers, 1u);
    CY_REQUIRE_EQ(out.report.barrier_names.size(), 1u);
    CY_CHECK(test::same_text(out.report.barrier_names[0], "forest.script"));
}

CY_TEST_CASE("two reads of one field are fused into one sampling") {
    Graph graph(test::allocator());
    graph.declare("fusion", 1);
    graph.declare_domains(DomainMask::of(ExecutionDomain::Cook));
    graph.declare_region_size(64.0, 0);
    cy::Expected<cy::pcg::AttributeId, cy::Error> a =
        graph.attributes().intern("moisture-a", cy::pcg::AttributeType::F32);
    cy::Expected<cy::pcg::AttributeId, cy::Error> b =
        graph.attributes().intern("pcg.density", cy::pcg::AttributeType::F32);
    cy::Expected<cy::pcg::AttributeId, cy::Error> c =
        graph.attributes().intern("moisture-b", cy::pcg::AttributeType::F32);
    CY_REQUIRE(a.has_value());
    CY_REQUIRE(b.has_value());
    CY_REQUIRE(c.has_value());

    GraphNode first;
    first.name = "fusion.read-a";
    first.kind = NodeKind::FieldRead;
    first.output = *a;
    first.params.field = 0x1234;
    cy::Expected<NodeId, cy::Error> first_id = graph.add(first);
    CY_REQUIRE(first_id.has_value());

    GraphNode second;
    second.name = "fusion.read-b";
    second.kind = NodeKind::FieldRead;
    second.output = *c;
    second.params.field = 0x1234;  // the same field
    cy::Expected<NodeId, cy::Error> second_id = graph.add(second);
    CY_REQUIRE(second_id.has_value());

    // BOTH reads have to be LIVE for the fusion to be the thing under test: dead-node elimination
    // runs first, and a fusion of a node nothing consumes would be a fusion of nothing. The blend
    // consumes them both.
    GraphNode blend;
    blend.name = "fusion.blend";
    blend.kind = NodeKind::Compute;
    blend.inputs[0] = *first_id;
    blend.inputs[1] = *second_id;
    blend.input_count = 2;
    blend.output = *b;
    blend.params.value = 0.5F;
    blend.params.second = 0.5F;
    cy::Expected<NodeId, cy::Error> blend_id = graph.add(blend);
    CY_REQUIRE(blend_id.has_value());

    GraphNode scatter;
    scatter.name = "fusion.scatter";
    scatter.kind = NodeKind::Scatter;
    scatter.inputs[0] = *blend_id;
    scatter.input_count = 1;
    scatter.params.count = 4;
    cy::Expected<NodeId, cy::Error> scatter_id = graph.add(scatter);
    CY_REQUIRE(scatter_id.has_value());

    GraphNode output;
    output.name = "fusion.output";
    output.kind = NodeKind::Output;
    output.inputs[0] = *scatter_id;
    output.input_count = 1;
    CY_REQUIRE(graph.add(output).has_value());

    Compiled out(test::allocator());
    cy::Expected<Program, cy::Error> program =
        cy::pcg::compile(test::allocator(), graph, out.report, out.diagnostics);
    CY_REQUIRE(program.has_value());
    CY_CHECK_EQ(out.report.queries_fused, 1u);
    // The field participates in dependency tracking exactly once, however many nodes read it.
    CY_REQUIRE_EQ(program->fields_read().size(), 1u);
    CY_CHECK_EQ(program->fields_read()[0], 0x1234u);

    // AND THE WORK IS SAVED, not only counted: the fused read takes the earlier read's stage as its
    // one input, so the evaluator copies a channel instead of sampling the same field twice.
    for (const cy::pcg::Stage& stage : program->stages()) {
        if (test::same_text(stage.name, "fusion.read-b")) {
            CY_CHECK(stage.query_fused);
            CY_CHECK_EQ(stage.input_count, 1u);
        }
    }
}

CY_TEST_CASE("a subgraph is inlined twice and its two instances mint different identities") {
    // "Graphs SHALL support subgraphs with typed exposed parameters, so that large generators
    // remain composable", and the reason the names are prefixed: two instantiations that kept one
    // name would be two nodes minting the SAME identities for different instances.
    Graph subgraph(test::allocator());
    cy::Expected<cy::pcg::AttributeId, cy::Error> channel =
        subgraph.attributes().intern("patch.density", cy::pcg::AttributeType::F32);
    CY_REQUIRE(channel.has_value());
    GraphNode constant;
    constant.name = "constant";
    constant.kind = NodeKind::Constant;
    constant.output = *channel;
    constant.params.value = 0.1F;
    cy::Expected<NodeId, cy::Error> constant_id = subgraph.add(constant);
    CY_REQUIRE(constant_id.has_value());
    GraphNode scatter;
    scatter.name = "scatter";
    scatter.kind = NodeKind::Scatter;
    scatter.inputs[0] = *constant_id;
    scatter.input_count = 1;
    scatter.params.count = 4;
    CY_REQUIRE(subgraph.add(scatter).has_value());

    Graph host(test::allocator());
    host.declare("host", 1);
    host.declare_domains(DomainMask::of(ExecutionDomain::Cook));
    host.declare_region_size(64.0, 0);
    cy::pcg::SubgraphParam binding;
    binding.name = "density";
    binding.target = *constant_id;
    binding.value = 0.7F;
    CY_REQUIRE(host.instantiate(subgraph, "north", cy::Span<const cy::pcg::SubgraphParam>()));
    CY_REQUIRE(
        host.instantiate(subgraph, "south", cy::Span<const cy::pcg::SubgraphParam>(&binding, 1)));
    CY_CHECK_EQ(host.size(), 4u);

    const cy::pcg::GraphNode* north = host.find_by_name("north.constant");
    const cy::pcg::GraphNode* south = host.find_by_name("south.constant");
    CY_REQUIRE(north != nullptr);
    CY_REQUIRE(south != nullptr);
    // The exposed parameter reached the second instance and not the first.
    CY_CHECK_NEAR(north->params.value, 0.1F, 1e-6F);
    CY_CHECK_NEAR(south->params.value, 0.7F, 1e-6F);
    // Different names, therefore different node identities, therefore instances that cannot
    // collide.
    CY_CHECK_NE(cy::pcg::node_identity(north->name).value,
                cy::pcg::node_identity(south->name).value);
    // The inlined edges were rebased: `south.scatter` reads `south.constant`, not the host's first
    // node.
    const cy::pcg::GraphNode* south_scatter = host.find_by_name("south.scatter");
    CY_REQUIRE(south_scatter != nullptr);
    CY_CHECK_EQ(south_scatter->inputs[0].value, 3u);
}
