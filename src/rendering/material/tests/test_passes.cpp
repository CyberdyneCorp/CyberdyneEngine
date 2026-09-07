// The optimisation pipeline, one switch at a time. M7 task 6.2.
//
// `material-compiler`: "Each pass SHALL be individually disableable in development builds, so a
// suspected miscompilation can be bisected." design.md §1.4 measured what that costs — eight of the
// spike's nine switches changed the compiled program — and the table below is this implementation's
// own measurement of the same question, which is not identical and says so.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/material/compiler.h>
#include <cy/test/test.h>

#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "fixtures.h"

using namespace cy;
using namespace cy::rendering::material;
using cy::rendering::material::testing::build_reference_graph;
using cy::rendering::material::testing::GraphIds;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

struct Compiled {
    u64 ir_digest = 0;
    u64 program_digest = 0;
    u32 nodes = 0;
    u32 statements = 0;
    u32 hoisted = 0;
};

[[nodiscard]] bool compile_reference(const PassSwitches& switches, Compiled& out) noexcept {
    MaterialGraph graph(allocator(), Name::intern("worn_metal"));
    GraphIds ids;
    if (!build_reference_graph(graph, ids)) {
        return false;
    }
    // The switches reach the FRONT-END's builder too. Interning happens there, so a switch the
    // front-end ignored could not bisect anything: the values a disabled pass is meant to keep
    // apart would already have been merged before the pipeline ran.
    auto authored = lower_graph(graph, allocator(), switches);
    if (!authored) {
        return false;
    }
    OptimiseReport report(allocator());
    auto optimised = optimise(authored.value(), switches, report);
    if (!optimised) {
        return false;
    }
    EmitOptions options;
    options.canonical_order = switches.canonical_emission_order;
    options.hoist_uniform = switches.uniform_varying;
    options.shading_model =
        cy::render::shading_model_name(match_shading_model(closure_set(optimised.value())).model);
    auto source = emit_program(optimised.value(), options);
    if (!source) {
        return false;
    }
    out.ir_digest = optimised.value().digest();
    out.nodes = optimised.value().size();
    out.program_digest = source.value().digest;
    out.statements = source.value().statements;
    out.hoisted = source.value().hoisted_statements;
    return true;
}

struct SwitchRow {
    const char* name;
    bool PassSwitches::* member;
    /// What turning it off does to the emitted program.
    bool changes_program;
    /// What it does to the module.
    bool changes_module;
};

}  // namespace

CY_TEST_CASE("material_passes: every switch is load-bearing, and the one that is not says why") {
    Compiled shipping;
    CY_REQUIRE(compile_reference(PassSwitches{}, shipping));

    // The measured table for THIS implementation. It differs from design.md §1.4's in one row and
    // the difference is a property of the emitter, not a weaker compiler: emission walks the
    // canonical order FROM THE ROOTS, so an orphan cannot reach the generated source however many
    // of them the module carries. Dead-node elimination therefore changes the module — the node
    // count, and the drop report an editor greys nodes out with — and not the program. The spike's
    // emitter walked the module, which is why its row differs.
    const SwitchRow rows[] = {
        {"interning", &PassSwitches::interning, true, true},
        {"canonical_commutative", &PassSwitches::canonical_commutative, true, true},
        {"constant_folding", &PassSwitches::constant_folding, true, true},
        {"common_subexpression", &PassSwitches::common_subexpression, true, true},
        {"texture_sample_dedup", &PassSwitches::texture_sample_dedup, true, true},
        {"closure_simplification", &PassSwitches::closure_simplification, true, true},
        {"dead_node_elimination", &PassSwitches::dead_node_elimination, false, true},
        {"uniform_varying", &PassSwitches::uniform_varying, true, false},
        {"canonical_emission_order", &PassSwitches::canonical_emission_order, true, false},
    };

    u32 changing_the_program = 0;
    for (const SwitchRow& row : rows) {
        PassSwitches switches;
        switches.*row.member = false;
        CY_CHECK_FALSE(switches.all_enabled());
        CY_CHECK(std::strcmp(switches.first_disabled(), row.name) == 0);

        Compiled bisected;
        CY_REQUIRE(compile_reference(switches, bisected));
        const bool program_changed = bisected.program_digest != shipping.program_digest;
        const bool module_changed =
            bisected.ir_digest != shipping.ir_digest || bisected.nodes != shipping.nodes;
        const std::string subject = std::string("switch off: ") + row.name;
        CY_TEST_MESSAGE(subject);
        CY_CHECK_EQ(program_changed, row.changes_program);
        CY_CHECK_EQ(module_changed, row.changes_module);
        changing_the_program += program_changed ? 1U : 0U;
    }
    CY_CHECK_EQ(changing_the_program, 8U);
}

CY_TEST_CASE("material_passes: a bisection build says it is not the shipping material") {
    MaterialGraph graph(allocator(), Name::intern("worn_metal"));
    GraphIds ids;
    CY_REQUIRE(build_reference_graph(graph, ids));
    auto authored = lower_graph(graph, allocator());
    CY_REQUIRE(authored.has_value());

    CompileOptions options;
    options.derive_family = false;
    options.derive_tiers = false;
    options.passes.constant_folding = false;
    auto compiled = compile_material(authored.value(), options, allocator());
    CY_REQUIRE(compiled.has_value());
    CY_CHECK(compiled.value().optimisation().bisection_build);

    bool warned = false;
    for (const CompileDiagnostic& diagnostic : compiled.value().diagnostics()) {
        warned = warned || std::strcmp(diagnostic.code, "bisection-build") == 0;
    }
    CY_CHECK(warned);
}

CY_TEST_CASE("material_passes: three identical samples collapse to one") {
    // "WHEN a graph samples one texture with identical coordinates in three places THEN the
    // compiled program SHALL contain one sample."
    MaterialGraph graph(allocator(), Name::intern("triple_sample"));
    CY_REQUIRE(
        graph.declare_texture(TextureDecl{Name::intern("albedo"), Immediate{}, false}).has_value());
    const u8 components[] = {0, 1, 2};
    Immediate mask;
    mask.mask = Builder::swizzle_mask(Span<const u8>(components, 3));

    auto uv = graph.add(GraphOp::Attribute, Name::intern("uv0"), ValueType::Vec2);
    CY_REQUIRE(uv.has_value());
    u32 previous = kInvalidNode;
    for (u32 index = 0; index < 3U; ++index) {
        auto sample = graph.add(GraphOp::TextureSample, Name::intern("albedo"), ValueType::Vec4);
        CY_REQUIRE(sample.has_value());
        CY_REQUIRE(graph.connect(uv.value(), sample.value(), 0).has_value());
        auto rgb = graph.add(GraphOp::Swizzle, Name{}, ValueType::Count, mask);
        CY_REQUIRE(rgb.has_value());
        CY_REQUIRE(graph.connect(sample.value(), rgb.value(), 0).has_value());
        if (previous == kInvalidNode) {
            previous = rgb.value();
            continue;
        }
        auto sum = graph.add(GraphOp::Add);
        CY_REQUIRE(sum.has_value());
        CY_REQUIRE(graph.connect(previous, sum.value(), 0).has_value());
        CY_REQUIRE(graph.connect(rgb.value(), sum.value(), 1).has_value());
        previous = sum.value();
    }
    auto closure = graph.add(GraphOp::Diffuse);
    CY_REQUIRE(closure.has_value());
    CY_REQUIRE(graph.connect(previous, closure.value(), 0).has_value());
    CY_REQUIRE(graph.set_surface_output(closure.value()).has_value());

    auto authored = lower_graph(graph, allocator());
    CY_REQUIRE(authored.has_value());
    OptimiseReport report(allocator());
    auto optimised = optimise(authored.value(), PassSwitches{}, report);
    CY_REQUIRE(optimised.has_value());

    u32 samples = 0;
    for (NodeId id = 0; id < optimised.value().size(); ++id) {
        samples += optimised.value().node(id).op == Op::TextureSample ? 1U : 0U;
    }
    CY_CHECK_EQ(samples, 1U);
    CY_CHECK_EQ(report.duplicate_samples_removed, 2U);

    // With the switch off, all three survive — which is what makes the switch a bisection tool
    // rather than a comment.
    PassSwitches without;
    without.texture_sample_dedup = false;
    auto unmerged = lower_graph(graph, allocator(), without);
    CY_REQUIRE(unmerged.has_value());
    OptimiseReport second(allocator());
    auto duplicated = optimise(unmerged.value(), without, second);
    CY_REQUIRE(duplicated.has_value());
    samples = 0;
    for (NodeId id = 0; id < duplicated.value().size(); ++id) {
        samples += duplicated.value().node(id).op == Op::TextureSample ? 1U : 0U;
    }
    CY_CHECK_EQ(samples, 3U);
}

CY_TEST_CASE("material_passes: a uniform subexpression is hoisted, and a varying one is not") {
    // "WHEN a subexpression depends only on material parameters THEN it SHALL be evaluated once
    // into parameter data rather than per pixel." `1 - metallic` is that subexpression; every
    // statement below it in the reference material samples a texture.
    Compiled shipping;
    CY_REQUIRE(compile_reference(PassSwitches{}, shipping));
    CY_CHECK_GT(shipping.hoisted, 0U);
    CY_CHECK_LT(shipping.hoisted, shipping.statements);

    PassSwitches without;
    without.uniform_varying = false;
    Compiled flat;
    CY_REQUIRE(compile_reference(without, flat));
    CY_CHECK_EQ(flat.hoisted, 0U);
    CY_CHECK_EQ(flat.statements, shipping.statements);
}

CY_TEST_CASE("material_passes: a custom node takes part in elimination like any other") {
    // "WHEN a custom expression node's output is unused THEN it SHALL be eliminated like any other
    // node." A custom node is Slang the compiler does not read; that is exactly why it must not be
    // a hole in the passes — an escape hatch nothing can eliminate is an escape hatch that grows.
    // A custom delimiter: the material's own text contains `)"`, which would close a plain one.
    constexpr const char* kCustom = R"MATERIAL(
material custom_node {
    param base : float3 = (0.5, 0.5, 0.5);
    param k    : float  = 2.0;
    let boosted = custom("pow($0, float3($1, $1, $1))", base, k);
    let unused  = custom("cy_never_called($0)", k);
    surface = diffuse(boosted);
    opacity = 1.0;
}
)MATERIAL";
    ParseDiagnostic diagnostic(allocator());
    auto authored = parse_material(kCustom, allocator(), diagnostic);
    CY_REQUIRE(authored.has_value());
    OptimiseReport report(allocator());
    auto optimised = optimise(authored.value(), PassSwitches{}, report);
    CY_REQUIRE(optimised.has_value());
    CY_CHECK_GT(report.dropped_nodes, 0U);

    EmitOptions options;
    auto source = emit_program(optimised.value(), options);
    CY_REQUIRE(source.has_value());
    const std::string_view text = source.value().view();
    // The custom node that IS used is emitted, with its arguments substituted.
    CY_CHECK(text.find("pow(ctx.params.base, float3(ctx.params.k, ctx.params.k, ctx.params.k))") !=
             std::string_view::npos);
    // The one that is not used is gone.
    CY_CHECK(text.find("cy_never_called") == std::string_view::npos);
}

CY_TEST_CASE("material_passes: the pipeline reports what it removed, by authoring node") {
    MaterialGraph graph(allocator(), Name::intern("worn_metal"));
    GraphIds ids;
    CY_REQUIRE(build_reference_graph(graph, ids));
    auto authored = lower_graph(graph, allocator());
    CY_REQUIRE(authored.has_value());

    OptimiseReport report(allocator());
    auto optimised = optimise(authored.value(), PassSwitches{}, report);
    CY_REQUIRE(optimised.has_value());
    CY_CHECK_GT(report.dropped_nodes, 0U);
    CY_CHECK_GT(report.nodes_before, report.nodes_after);

    bool named = false;
    for (const u32 origin : report.dropped_origins) {
        named = named || origin == ids.orphan;
    }
    CY_CHECK(named);

    // With dead-node elimination off the orphan is carried across, and the switch is therefore not
    // a placebo — which is exactly what the spike found its own first draft to be.
    PassSwitches without;
    without.dead_node_elimination = false;
    OptimiseReport second(allocator());
    auto carried = optimise(authored.value(), without, second);
    CY_REQUIRE(carried.has_value());
    CY_CHECK_GT(carried.value().size(), optimised.value().size());
}
