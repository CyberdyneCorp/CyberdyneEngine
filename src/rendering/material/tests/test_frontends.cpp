// The exit criterion: a graph and a hand-written material produce identical programs. Task 6.1.
//
// `material-compiler` — "Authoring forms share one representation": "WHEN a graph and a text
// definition describe the same material THEN they SHALL produce identical IR and an identical
// compiled program."
//
// The two front-ends are deliberately unalike (see fixtures.h). What makes them one material is the
// seven decisions in ir.h, and each case below fails if one of them is removed — which is why the
// pass suite switches them off one at a time and watches this equality break.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/material/compiler.h>
#include <cy/test/test.h>

#include "fixtures.h"

using namespace cy;
using namespace cy::rendering::material;
using cy::rendering::material::testing::build_reference_graph;
using cy::rendering::material::testing::GraphIds;
using cy::rendering::material::testing::reference_text;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

/// Both front-ends, run to the same point: authored module, then the optimisation pipeline.
struct BothWays {
    explicit BothWays(Allocator& memory) noexcept
        : graph_report(memory), text_report(memory), from_graph(memory), from_text(memory) {}

    OptimiseReport graph_report;
    OptimiseReport text_report;
    Module from_graph;
    Module from_text;
    u32 graph_authored_nodes = 0;
    u32 text_authored_nodes = 0;
    GraphIds ids;

    [[nodiscard]] bool run(const PassSwitches& switches) noexcept {
        MaterialGraph graph(allocator(), Name::intern("worn_metal"));
        if (!build_reference_graph(graph, ids)) {
            return false;
        }
        auto graph_module = lower_graph(graph, allocator());
        if (!graph_module) {
            return false;
        }
        graph_authored_nodes = graph_module.value().size();

        ParseDiagnostic diagnostic(allocator());
        auto text_module = parse_material(reference_text(), allocator(), diagnostic);
        if (!text_module) {
            return false;
        }
        text_authored_nodes = text_module.value().size();

        auto optimised_graph = optimise(graph_module.value(), switches, graph_report);
        auto optimised_text = optimise(text_module.value(), switches, text_report);
        if (!optimised_graph || !optimised_text) {
            return false;
        }
        from_graph = std::move(optimised_graph.value());
        from_text = std::move(optimised_text.value());
        return true;
    }
};

[[nodiscard]] Expected<GeneratedSource, Error> emit(const Module& module) noexcept {
    EmitOptions options;
    options.shading_model =
        cy::render::shading_model_name(match_shading_model(closure_set(module)).model);
    return emit_program(module, options);
}

}  // namespace

CY_TEST_CASE("material_frontends: a graph and a text definition are one material") {
    BothWays both(allocator());
    CY_REQUIRE(both.run(PassSwitches{}));

    // Before the compiler runs they are two different things, which is what makes the equality
    // afterwards a statement about the compiler rather than about the fixture.
    CY_CHECK_NE(both.graph_authored_nodes, both.text_authored_nodes);

    CY_CHECK_EQ(both.from_graph.digest(), both.from_text.digest());
    CY_CHECK_EQ(both.from_graph.size(), both.from_text.size());

    auto graph_source = emit(both.from_graph);
    auto text_source = emit(both.from_text);
    CY_REQUIRE(graph_source.has_value());
    CY_REQUIRE(text_source.has_value());
    CY_CHECK_EQ(graph_source.value().digest, text_source.value().digest);
    CY_CHECK(graph_source.value().view() == text_source.value().view());
}

CY_TEST_CASE("material_frontends: sine node and text call emit the same shader operation") {
    MaterialGraph graph(allocator(), Name::intern("sway"));
    ParameterDecl phase;
    phase.name = Name::intern("phase");
    phase.type = ValueType::Float;
    phase.default_value = Immediate::scalar(0.5F);
    CY_REQUIRE(graph.declare_parameter(phase));
    auto input = graph.add(GraphOp::Parameter, phase.name);
    auto sine = graph.add(GraphOp::Sin);
    auto tint =
        graph.add(GraphOp::Constant, Name{}, ValueType::Vec3, Immediate{1.0F, 1.0F, 1.0F, 0.0F, 0});
    auto scaled = graph.add(GraphOp::Multiply);
    auto surface = graph.add(GraphOp::Emission);
    CY_REQUIRE(input.has_value());
    CY_REQUIRE(sine.has_value());
    CY_REQUIRE(tint.has_value());
    CY_REQUIRE(scaled.has_value());
    CY_REQUIRE(surface.has_value());
    CY_REQUIRE(graph.connect(input.value(), sine.value(), 0));
    CY_REQUIRE(graph.connect(tint.value(), scaled.value(), 0));
    CY_REQUIRE(graph.connect(sine.value(), scaled.value(), 1));
    CY_REQUIRE(graph.connect(scaled.value(), surface.value(), 0));
    CY_REQUIRE(graph.set_surface_output(surface.value()));

    auto from_graph = lower_graph(graph, allocator());
    CY_REQUIRE(from_graph.has_value());
    ParseDiagnostic diagnostic(allocator());
    auto from_text = parse_material(
        "material sway { param phase : float = 0.5; "
        "surface = emission((1.0, 1.0, 1.0) * sin(phase)); }",
        allocator(), diagnostic);
    CY_REQUIRE(from_text.has_value());
    OptimiseReport graph_report(allocator());
    OptimiseReport text_report(allocator());
    auto graph_ir = optimise(from_graph.value(), PassSwitches{}, graph_report);
    auto text_ir = optimise(from_text.value(), PassSwitches{}, text_report);
    CY_REQUIRE(graph_ir.has_value());
    CY_REQUIRE(text_ir.has_value());
    CY_CHECK_EQ(graph_ir.value().digest(), text_ir.value().digest());

    CompileOptions options;
    options.derive_family = false;
    options.derive_tiers = false;
    auto compiled = compile_material(from_graph.value(), options, allocator());
    CY_REQUIRE(compiled.has_value());
    const CompiledProgram* primary = compiled.value().find(ProgramKind::Primary, QualityTier::High);
    CY_REQUIRE(primary != nullptr);
    CY_CHECK(primary->source.view().find("sin(") != std::string_view::npos);

    MaterialGraph vector_graph(allocator(), Name::intern("vector_sway"));
    ParameterDecl vector_phase;
    vector_phase.name = Name::intern("phase3");
    vector_phase.type = ValueType::Vec3;
    vector_phase.default_value = Immediate{0.5F, 0.25F, 0.0F, 0.0F, 0};
    CY_REQUIRE(vector_graph.declare_parameter(vector_phase));
    auto vector_input = vector_graph.add(GraphOp::Parameter, vector_phase.name, ValueType::Vec3);
    auto vector_sine = vector_graph.add(GraphOp::Sin);
    auto vector_surface = vector_graph.add(GraphOp::Emission);
    CY_REQUIRE(vector_input.has_value());
    CY_REQUIRE(vector_sine.has_value());
    CY_REQUIRE(vector_surface.has_value());
    CY_REQUIRE(vector_graph.connect(vector_input.value(), vector_sine.value(), 0));
    CY_REQUIRE(vector_graph.connect(vector_sine.value(), vector_surface.value(), 0));
    CY_REQUIRE(vector_graph.set_surface_output(vector_surface.value()));
    auto vector_ir = lower_graph(vector_graph, allocator());
    CY_REQUIRE(vector_ir.has_value());
    ParseDiagnostic vector_diagnostic(allocator());
    auto vector_text = parse_material(
        "material vector_sway { param phase3 : float3 = (0.5, 0.25, 0.0); "
        "surface = emission(sin(phase3)); }",
        allocator(), vector_diagnostic);
    CY_REQUIRE(vector_text.has_value());
    OptimiseReport vector_graph_report(allocator());
    OptimiseReport vector_text_report(allocator());
    auto vector_graph_ir = optimise(vector_ir.value(), PassSwitches{}, vector_graph_report);
    auto vector_text_ir = optimise(vector_text.value(), PassSwitches{}, vector_text_report);
    CY_REQUIRE(vector_graph_ir.has_value());
    CY_REQUIRE(vector_text_ir.has_value());
    CY_CHECK_EQ(vector_graph_ir.value().digest(), vector_text_ir.value().digest());
}

CY_TEST_CASE("material_frontends: provenance does not change the program") {
    // design.md §1.2 decision 4. The graph carries an origin on every value and the text carries
    // none, and the two programs are byte-identical — so attribution is genuinely a side table.
    BothWays both(allocator());
    CY_REQUIRE(both.run(PassSwitches{}));

    u32 with_origins = 0;
    for (NodeId id = 0; id < both.from_graph.size(); ++id) {
        with_origins += both.from_graph.origins(id).empty() ? 0U : 1U;
    }
    CY_CHECK_GT(with_origins, 0U);
    for (NodeId id = 0; id < both.from_text.size(); ++id) {
        CY_CHECK(both.from_text.origins(id).empty());
    }
    CY_CHECK_EQ(both.from_graph.digest(), both.from_text.digest());
}

CY_TEST_CASE(
    "material_frontends: the editor's warts are removed by the compiler and not by the fixture") {
    BothWays both(allocator());
    CY_REQUIRE(both.run(PassSwitches{}));

    // A muted closure, two weight ports of one, a duplicated texture sample and a disconnected
    // node all arrive in the IR and all leave it.
    CY_CHECK_GT(both.graph_report.simplified_closures, 0U);
    CY_CHECK_GT(both.graph_report.duplicate_samples_removed, 0U);
    CY_CHECK_GT(both.graph_report.dropped_nodes, 0U);

    bool orphan_reported = false;
    for (const u32 origin : both.graph_report.dropped_origins) {
        orphan_reported = orphan_reported || origin == both.ids.orphan;
    }
    CY_CHECK(orphan_reported);

    // One sample of `base_color_map` survives, though the graph dragged it in twice.
    u32 samples = 0;
    for (NodeId id = 0; id < both.from_graph.size(); ++id) {
        samples += both.from_graph.node(id).op == Op::TextureSample ? 1U : 0U;
    }
    CY_CHECK_EQ(samples, 2U);
}

CY_TEST_CASE("material_frontends: the pipeline reaches a fixed point") {
    BothWays both(allocator());
    CY_REQUIRE(both.run(PassSwitches{}));
    CY_CHECK_LE(both.graph_report.iterations, kMaxOptimiseIterations);
    CY_CHECK_GE(both.graph_report.iterations, 2U);

    // Running it again over its own output changes nothing: that is what a fixed point is, and it
    // is what lets a pass be added without asking whether the pipeline still converges.
    OptimiseReport again(allocator());
    auto settled = optimise(both.from_graph, PassSwitches{}, again);
    CY_REQUIRE(settled.has_value());
    CY_CHECK_EQ(settled.value().digest(), both.from_graph.digest());
    CY_CHECK_EQ(settled.value().size(), both.from_graph.size());
}

CY_TEST_CASE("material_frontends: a parse error names its line") {
    ParseDiagnostic diagnostic(allocator());
    auto parsed =
        parse_material("material broken { surface = diffuse(nope); }", allocator(), diagnostic);
    CY_CHECK_FALSE(parsed.has_value());
    CY_CHECK_EQ(diagnostic.line, 1U);
    CY_CHECK_FALSE(diagnostic.empty());
}
