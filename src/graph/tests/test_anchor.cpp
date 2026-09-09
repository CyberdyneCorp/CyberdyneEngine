// THE ANCHOR. M8.b task 2.3.
//
// design.md §1.7: the generalised expression core, "given the material op table and type lattice as
// a domain, must reproduce the tree's own reference material at IR digest f48f3faf395e52fd,
// post-pipeline digest 178a3630921e0506 (16 nodes, 3 dropped, 10 merged, 1 folded) and program
// digest 7f74500626ea001a. A core that hits all three has lost nothing the material compiler
// depends on, and porting the material compiler onto it becomes a later mechanical change rather
// than a risk inside this milestone."
//
// The three numbers are the M8.b spike's P13, measured against `src/rendering/material/` itself.
// They are written out below as literals rather than computed, because a criterion that recomputes
// its own expectation checks nothing.

#include <cy/core/memory/system_allocator.h>
#include <cy/graph/emit.h>
#include <cy/graph/passes.h>
#include <cy/test/test.h>

#include "anchor/material_anchor.h"

using namespace cy;
using namespace cy::graph;
using namespace cy::graph::anchor;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

/// The three digests the M8.b spike measured, probe P13.
constexpr u64 kAnchorIr = 0xf48f3faf395e52fdULL;
constexpr u64 kAnchorPipeline = 0x178a3630921e0506ULL;
constexpr u64 kAnchorProgram = 0x7f74500626ea001aULL;

}  // namespace

CY_TEST_CASE("graph_anchor: the reference material reaches the measured IR digest") {
    MaterialGraph graph(allocator(), Name::intern("worn_metal"));
    ReferenceIds ids{};
    CY_REQUIRE(build_reference_graph(graph, ids));
    CY_CHECK_EQ(graph.nodes().size(), 26U);

    auto module = lower_graph(graph, allocator());
    CY_REQUIRE(module.has_value());
    CY_CHECK_EQ(module.value().size(), 26U);
    CY_CHECK_EQ(module.value().digest(), kAnchorIr);
}

CY_TEST_CASE("graph_anchor: the pipeline reaches the measured post-pipeline digest and counts") {
    MaterialGraph graph(allocator(), Name::intern("worn_metal"));
    ReferenceIds ids{};
    CY_REQUIRE(build_reference_graph(graph, ids));
    auto module = lower_graph(graph, allocator());
    CY_REQUIRE(module.has_value());

    OptimiseReport report(allocator());
    const PassSwitches switches;
    auto optimised = optimise(module.value(), switches, report);
    CY_REQUIRE(optimised.has_value());

    CY_CHECK_EQ(optimised.value().size(), 16U);
    CY_CHECK_EQ(optimised.value().digest(), kAnchorPipeline);
    CY_CHECK_EQ(report.dropped_nodes, 3U);
    CY_CHECK_EQ(report.merged_values, 10U);
    CY_CHECK_EQ(report.folded_constants, 1U);
    CY_CHECK_FALSE(report.bisection_build);

    // The drop report is answerable BY AUTHORING NODE, which is what an editor has. The orphan
    // multiply, its constant, and the muted emission are what the rebuild could not reach.
    bool saw_orphan = false;
    bool saw_orphan_constant = false;
    for (const u32 origin : report.dropped_origins) {
        saw_orphan = saw_orphan || origin == ids.orphan;
        saw_orphan_constant = saw_orphan_constant || origin == ids.orphan_constant;
    }
    CY_CHECK(saw_orphan);
    CY_CHECK(saw_orphan_constant);
}

CY_TEST_CASE("graph_anchor: the emitted program reaches the measured program digest") {
    MaterialGraph graph(allocator(), Name::intern("worn_metal"));
    ReferenceIds ids{};
    CY_REQUIRE(build_reference_graph(graph, ids));
    auto module = lower_graph(graph, allocator());
    CY_REQUIRE(module.has_value());
    OptimiseReport report(allocator());
    const PassSwitches switches;
    auto optimised = optimise(module.value(), switches, report);
    CY_REQUIRE(optimised.has_value());

    const EmitOptions options;
    auto source = emit_program(optimised.value(), material_target(), options);
    CY_REQUIRE(source.has_value());
    CY_CHECK_EQ(source.value().statements, 11U);
    CY_CHECK_EQ(source.value().digest, kAnchorProgram);
    // The debug map: one entry per statement, each naming the IR node the line came from.
    CY_CHECK_EQ(source.value().value_nodes.size(), source.value().statements);
}

CY_TEST_CASE("graph_anchor: the pipeline reaches a fixed point and stays there") {
    MaterialGraph graph(allocator(), Name::intern("worn_metal"));
    ReferenceIds ids{};
    CY_REQUIRE(build_reference_graph(graph, ids));
    auto module = lower_graph(graph, allocator());
    CY_REQUIRE(module.has_value());

    OptimiseReport first(allocator());
    const PassSwitches switches;
    auto once = optimise(module.value(), switches, first);
    CY_REQUIRE(once.has_value());
    OptimiseReport second(allocator());
    auto twice = optimise(once.value(), switches, second);
    CY_REQUIRE(twice.has_value());

    // Running the pipeline over its own output changes nothing: that is what "a pass's output is in
    // the same canonical form as its input" means, and it is why the loop can terminate on a
    // digest.
    CY_CHECK_EQ(twice.value().digest(), once.value().digest());
    CY_CHECK_EQ(twice.value().size(), once.value().size());
    CY_CHECK_LE(first.iterations, kMaxOptimiseIterations);
}
